using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace PS5Upload
{
    /// <summary>
    /// Minimal HTTP/1.1 file server for on-the-fly PKG installs. The PS5 payload's
    /// localhost responder (:13801/remote) proxies ranged GETs to this server, so the
    /// console installer streams the PKG straight from the PC — nothing is staged on
    /// the console's disk. Implemented with raw TcpListener (no HTTP.sys URL ACL needed).
    /// Supports HEAD and GET with Range (what the install daemon uses).
    /// </summary>
    public sealed class PkgHttpServer : IDisposable
    {
        private readonly TcpListener _listener;
        private readonly ConcurrentDictionary<string, string> _files = new(); // URL path -> local file
        private readonly CancellationTokenSource _cts = new();

        public int Port { get; }

        public PkgHttpServer()
        {
            TcpListener? l = null;
            int port = 18990;
            for (; port < 19010; port++)
            {
                try { l = new TcpListener(IPAddress.Any, port); l.Start(); break; }
                catch (SocketException) { }
            }
            if (l == null) throw new IOException("No free port for PKG HTTP server");
            _listener = l;
            Port = port;
            _ = AcceptLoopAsync();
        }

        /// <summary>Register a local file; returns the URL path (escaped) to request it by.
        /// Keys are stored UNescaped because requests are unescaped before lookup.</summary>
        public string AddFile(string localPath)
        {
            var name = Path.GetFileName(localPath);
            _files["/" + name] = localPath;
            return "/" + Uri.EscapeDataString(name);
        }

        /// <summary>Optional per-request logger (hits, misses, ranges) wired to the app log.</summary>
        public Action<string>? OnRequest;

        public void RemoveFile(string urlPath) => _files.TryRemove(urlPath, out _);

        private async Task AcceptLoopAsync()
        {
            while (!_cts.IsCancellationRequested)
            {
                TcpClient c;
                try { c = await _listener.AcceptTcpClientAsync(); }
                catch { break; }
                _ = Task.Run(() => HandleAsync(c));
            }
        }

        private int _reqCount;

        private async Task HandleAsync(TcpClient c)
        {
            try
            {
                using (c)
                {
                    c.ReceiveTimeout = 15000;
                    c.SendTimeout = 60000;
                    c.NoDelay = true;
                    c.SendBufferSize = 4 * 1024 * 1024;   // bigger socket buffer → fewer context switches
                    var ns = c.GetStream();
                    var hdr = new byte[8192];
                    int filled = 0;   // bytes currently in hdr (pipelined leftovers carry over)

                    // Keep-alive loop: the payload proxy reuses this connection for
                    // sequential range requests — one TCP setup serves the whole PKG.
                    while (!_cts.IsCancellationRequested)
                    {
                        // Read request headers (cap 8KB), with a 15s idle timeout.
                        int hend = -1;
                        using (var idle = CancellationTokenSource.CreateLinkedTokenSource(_cts.Token))
                        {
                            idle.CancelAfter(15000);
                            try
                            {
                                while (hend < 0 && filled < hdr.Length)
                                {
                                    hend = IndexOf(hdr, filled, "\r\n\r\n");
                                    if (hend >= 0) break;
                                    int r = await ns.ReadAsync(hdr, filled, hdr.Length - filled, idle.Token);
                                    if (r <= 0) return;
                                    filled += r;
                                }
                                hend = IndexOf(hdr, filled, "\r\n\r\n");
                            }
                            catch (OperationCanceledException) { return; }
                            catch (IOException) { return; }
                        }
                        if (hend < 0) return;   // oversized/garbage request
                        var req = Encoding.ASCII.GetString(hdr, 0, hend);
                        // Shift leftover bytes (next request) to buffer start.
                        int consumed = hend + 4;
                        filled -= consumed;
                        if (filled > 0) Buffer.BlockCopy(hdr, consumed, hdr, 0, filled);

                        var parts = req.Split("\r\n")[0].Split(' ');
                        if (parts.Length < 2) return;
                        var method = parts[0].ToUpperInvariant();
                        var urlPath = Uri.UnescapeDataString(parts[1].Split('?')[0]);
                        bool clientClose = req.IndexOf("Connection: close", StringComparison.OrdinalIgnoreCase) >= 0;

                        if (method != "GET" && method != "HEAD" || !_files.TryGetValue(urlPath, out var localPath))
                        {
                            OnRequest?.Invoke($"404 {method} {urlPath}");
                            await WriteAsciiAsync(ns, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            return;
                        }

                        var fi = new FileInfo(localPath);
                        long total = fi.Length, start = 0, end = total - 1;
                        bool partial = false;

                        // Parse "Range: bytes=a-b" / "bytes=a-" / "bytes=-n".
                        var rangeIdx = req.IndexOf("\r\nRange:", StringComparison.OrdinalIgnoreCase);
                        if (rangeIdx < 0) rangeIdx = req.StartsWith("Range:", StringComparison.OrdinalIgnoreCase) ? 0 : -1;
                        if (rangeIdx >= 0)
                        {
                            var lineEnd = req.IndexOf("\r\n", rangeIdx + 1);
                            var spec = req.Substring(rangeIdx + 2 + 6, (lineEnd < 0 ? req.Length : lineEnd) - rangeIdx - 2 - 6).Trim();
                            if (spec.StartsWith("bytes=", StringComparison.OrdinalIgnoreCase))
                            {
                                var se = spec.Substring(6).Split('-');
                                if (se.Length == 2)
                                {
                                    if (se[0].Length == 0 && long.TryParse(se[1], out var suffix))
                                    {
                                        start = Math.Max(0, total - suffix);   // suffix range: last N bytes
                                        end = total - 1;
                                    }
                                    else
                                    {
                                        long.TryParse(se[0], out start);
                                        if (se[1].Length > 0 && long.TryParse(se[1], out var e2))
                                            end = Math.Min(e2, total - 1);
                                    }
                                    if (start < 0) start = 0;
                                    if (start > end || start >= total) { start = 0; end = total - 1; }
                                    partial = true;
                                }
                            }
                        }

                        long len = Math.Max(0, end - start + 1);
                        // Log the first few requests + every 64th — the daemon fires
                        // hundreds of range GETs, don't spam the UI with each one.
                        int rn = Interlocked.Increment(ref _reqCount);
                        if (rn <= 8 || (rn & 63) == 0)
                            OnRequest?.Invoke($"{method} {urlPath} bytes={start}-{end}/{total} [#{rn}]");
                        var sb = new StringBuilder();
                        sb.Append(partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n");
                        sb.Append("Content-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n");
                        sb.Append($"Content-Length: {len}\r\n");
                        if (partial) sb.Append($"Content-Range: bytes {start}-{end}/{total}\r\n");
                        sb.Append(clientClose ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n");
                        await WriteAsciiAsync(ns, sb.ToString());

                        if (method == "GET")
                        {
                            using var fs = new FileStream(localPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                            fs.Seek(start, SeekOrigin.Begin);
                            var buf = new byte[1024 * 1024];
                            long remain = len;
                            while (remain > 0)
                            {
                                int rd = await fs.ReadAsync(buf, 0, (int)Math.Min(buf.Length, remain), _cts.Token);
                                if (rd <= 0) break;
                                await ns.WriteAsync(buf, 0, rd, _cts.Token);
                                remain -= rd;
                            }
                        }
                        if (clientClose) return;
                    }
                }
            }
            catch { /* connection aborted by installer — fine */ }
        }

        private static int IndexOf(byte[] buf, int len, string needle)
        {
            var pat = Encoding.ASCII.GetBytes(needle);
            for (int i = 0; i + pat.Length <= len; i++)
            {
                bool m = true;
                for (int j = 0; j < pat.Length; j++) if (buf[i + j] != pat[j]) { m = false; break; }
                if (m) return i;
            }
            return -1;
        }

        private static async Task WriteAsciiAsync(NetworkStream ns, string s)
        {
            var b = Encoding.ASCII.GetBytes(s);
            await ns.WriteAsync(b, 0, b.Length);
        }

        public void Dispose()
        {
            _cts.Cancel();
            try { _listener.Stop(); } catch { }
        }
    }
}
