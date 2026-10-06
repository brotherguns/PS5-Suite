using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using SMBLibrary;
using SMBLibrary.Client;

namespace PS5Upload
{
    /// <summary>
    /// Managed SMB (NAS) access via SMBLibrary. One authenticated session per
    /// share root (\\server\share) is kept for the app lifetime; every file
    /// under that share reuses it. All store ops run under a per-session lock —
    /// SMBLibrary isn't thread-safe, so parallel upload lanes interleave their
    /// reads on the single session.
    /// </summary>
    public static class NasManager
    {
        public sealed class NasAuthException : IOException { public NasAuthException(string m) : base(m) { } }

        private sealed class Session
        {
            public string Server = "", Share = "";
            public string Domain = "", User = "", Pass = "";
            public SMB2Client? Client;
            public ISMBFileStore? Store;
            public readonly object Lock = new();
        }

        private static readonly ConcurrentDictionary<string, Session> _sessions = new(StringComparer.OrdinalIgnoreCase);

        /// A NAS connection the user chose to keep between app launches.
        /// Password is stored obfuscated (DPAPI on Windows, Base64 elsewhere)
        /// — convenient, not secure; never treat as a real credential vault.
        public sealed class SavedConnection
        {
            public string Path { get; set; } = "";      // e.g. \\192.168.0.5\G\games
            public string Server { get; set; } = "";
            public string Share { get; set; } = "";
            public string User { get; set; } = "";
            public string PassEnc { get; set; } = "";   // encoded, see Encode/DecodePass

            public string Display => string.IsNullOrEmpty(User) ? Path : $"{Path}   ({User})";
        }

        public static string EncodePass(string plain)
        {
            try
            {
                if (OperatingSystem.IsWindows())
                {
                    var enc = System.Security.Cryptography.ProtectedData.Protect(
                        System.Text.Encoding.UTF8.GetBytes(plain), null,
                        System.Security.Cryptography.DataProtectionScope.CurrentUser);
                    return "dpapi:" + Convert.ToBase64String(enc);
                }
            }
            catch { }
            return "b64:" + Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(plain));
        }

        public static string DecodePass(string enc)
        {
            try
            {
                if (enc.StartsWith("dpapi:") && OperatingSystem.IsWindows())
                {
                    var raw = System.Security.Cryptography.ProtectedData.Unprotect(
                        Convert.FromBase64String(enc.Substring(6)), null,
                        System.Security.Cryptography.DataProtectionScope.CurrentUser);
                    return System.Text.Encoding.UTF8.GetString(raw);
                }
                if (enc.StartsWith("b64:"))
                    return System.Text.Encoding.UTF8.GetString(Convert.FromBase64String(enc.Substring(4)));
            }
            catch { }
            return "";
        }

        // ============================================================
        // LAN DISCOVERY — scan local subnets for hosts listening on
        // TCP 445 (SMB). ~2s for a /24 with parallel probes.
        // ============================================================
        public sealed class NasHost
        {
            public string Ip = "";
            public string Name = "";
            public string Display => string.IsNullOrEmpty(Name) ? Ip : $"{Ip}  ({Name})";
        }

        public static async Task<List<NasHost>> DiscoverAsync(CancellationToken ct = default)
        {
            var ranges = GetLocalScanRanges();
            var found = new ConcurrentBag<string>();

            var probes = new List<Task>();
            using var sem = new SemaphoreSlim(220);
            foreach (var (network, mask) in ranges)
            {
                uint count = ~mask + 1;
                if (count > 2048) count = 2048; // sanity cap
                for (uint i = 1; i < count - 1; i++)
                {
                    uint ipN = network + i;
                    string ip = $"{(ipN >> 24) & 0xFF}.{(ipN >> 16) & 0xFF}.{(ipN >> 8) & 0xFF}.{ipN & 0xFF}";
                    probes.Add(Task.Run(async () =>
                    {
                        await sem.WaitAsync(ct).ConfigureAwait(false);
                        try
                        {
                            using var c = new System.Net.Sockets.TcpClient();
                            var t = c.ConnectAsync(ip, 445);
                            if (await Task.WhenAny(t, Task.Delay(350, ct)).ConfigureAwait(false) == t && c.Connected)
                                found.Add(ip);
                        }
                        catch { }
                        finally { sem.Release(); }
                    }, ct));
                }
            }
            try { await Task.WhenAll(probes).ConfigureAwait(false); } catch { }

            // Reverse-DNS the hits (parallel, short timeout, tolerate failures).
            var results = new ConcurrentBag<NasHost>();
            await Task.WhenAll(found.Select(async ip =>
            {
                string name = "";
                try
                {
                    var t = System.Net.Dns.GetHostEntryAsync(ip);
                    if (await Task.WhenAny(t, Task.Delay(1200, ct)).ConfigureAwait(false) == t)
                    {
                        var e = await t;
                        if (!string.IsNullOrEmpty(e.HostName) && e.HostName != ip) name = e.HostName;
                    }
                }
                catch { }
                results.Add(new NasHost { Ip = ip, Name = name });
            })).ConfigureAwait(false);

            return results.OrderBy(h => h.Ip).ToList();
        }

        /// Try to list a host's shares via OS tools: `net view` on Windows,
        /// `smbclient -L` elsewhere (if installed). Returns null when the
        /// tool isn't available or the output can't be parsed.
        public static List<string>? EnumerateSharesOs(string host, string user = "", string pass = "")
        {
            try
            {
                string exe, args;
                if (OperatingSystem.IsWindows())
                {
                    exe = "net"; args = $"view \\\\{host} /all";
                }
                else
                {
                    exe = "smbclient";
                    string u = string.IsNullOrEmpty(user) ? "-N" : $"-U {user}%{pass}";
                    args = $"-L //{host} {u} -g";   // -g = parseable output
                }
                var psi = new System.Diagnostics.ProcessStartInfo(exe, args)
                {
                    RedirectStandardOutput = true, RedirectStandardError = true,
                    UseShellExecute = false, CreateNoWindow = true
                };
                using var p = System.Diagnostics.Process.Start(psi);
                if (p == null) return null;
                string outp = p.StandardOutput.ReadToEnd();
                if (!p.WaitForExit(8000)) { try { p.Kill(); } catch { } return null; }

                var shares = new List<string>();
                if (OperatingSystem.IsWindows())
                {
                    // "Share name  Type  Used as  Comment" — names in col 0, e.g. "games   Disk"
                    foreach (var line in outp.Split('\n'))
                    {
                        var t = line.TrimEnd('\r');
                        if (t.Contains(" Disk") && t.TrimStart().Length > 0)
                        {
                            int i = t.IndexOf("  ");
                            string name = (i > 0 ? t.Substring(0, i) : t).Trim();
                            if (name.Length > 0 && !name.EndsWith("$") && !name.Equals("Share name", StringComparison.OrdinalIgnoreCase))
                                shares.Add(name);
                        }
                    }
                }
                else
                {
                    // smbclient -g lines: "Disk|sharename|comment"
                    foreach (var line in outp.Split('\n'))
                    {
                        var parts = line.Trim().Split('|');
                        if (parts.Length >= 2 && parts[0] == "Disk" && !parts[1].EndsWith("$"))
                            shares.Add(parts[1]);
                    }
                }
                return shares.Count > 0 ? shares : null;
            }
            catch { return null; }
        }

        /// Local IPv4 subnets to scan — (network, mask) as uints, big-endian.
        private static List<(uint network, uint mask)> GetLocalScanRanges()
        {
            var ranges = new List<(uint, uint)>();
            try
            {
                foreach (var nic in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (nic.OperationalStatus != System.Net.NetworkInformation.OperationalStatus.Up) continue;
                    if (nic.NetworkInterfaceType == System.Net.NetworkInformation.NetworkInterfaceType.Loopback) continue;
                    foreach (var ua in nic.GetIPProperties().UnicastAddresses)
                    {
                        if (ua.Address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork) continue;
                        var ipB = ua.Address.GetAddressBytes();
                        var mB = ua.IPv4Mask?.GetAddressBytes();
                        if (mB == null) continue;
                        uint ip = ((uint)ipB[0] << 24) | ((uint)ipB[1] << 16) | ((uint)ipB[2] << 8) | ipB[3];
                        uint mask = ((uint)mB[0] << 24) | ((uint)mB[1] << 16) | ((uint)mB[2] << 8) | mB[3];
                        ranges.Add((ip & mask, mask));
                    }
                }
            }
            catch { }
            // Fallback: if NIC enumeration gave nothing (odd platforms), guess common /24s is pointless — return empty.
            return ranges.Distinct().ToList();
        }

        public static bool IsUncPath(string? path) =>
            !string.IsNullOrEmpty(path) && (path.StartsWith("\\\\") || path.StartsWith("//"));

        public static string Normalize(string path)
        {
            path = path.Trim();
            if (path.StartsWith("//")) path = "\\\\" + path.Substring(2).Replace('/', '\\');
            return path;
        }

        /// "\\server\share" portion of a UNC path ("" if not UNC).
        public static string ShareRoot(string uncPath)
        {
            string p = Normalize(uncPath).TrimEnd('\\');
            int i = p.IndexOf('\\', 2);                    // end of server
            if (i < 0) return p;
            int j = p.IndexOf('\\', i + 1);               // end of share
            return j < 0 ? p : p.Substring(0, j);
        }

        /// Split \\server\share\a\b → (server, share, "a\b").
        public static bool TrySplit(string uncPath, out string server, out string share, out string rel)
        {
            server = share = rel = "";
            if (!IsUncPath(uncPath)) return false;
            string p = Normalize(uncPath);
            string rest = p.Substring(2);
            int i = rest.IndexOf('\\');
            if (i < 0) return false;
            server = rest.Substring(0, i);
            string rest2 = rest.Substring(i + 1);
            int j = rest2.IndexOf('\\');
            if (j < 0) { share = rest2; }
            else { share = rest2.Substring(0, j); rel = rest2.Substring(j + 1); }
            return server.Length > 0 && share.Length > 0;
        }

        public static bool HasSession(string uncPath) => _sessions.ContainsKey(ShareRoot(uncPath));

        /// Connect + authenticate + tree-connect to \\server\share. Throws
        /// NasAuthException on logon failure, IOException on other errors.
        public static void Connect(string server, string share, string user, string password, string domain = "")
        {
            var s = new Session { Server = server, Share = share, User = user, Pass = password, Domain = domain };
            Establish(s);
            _sessions["\\\\" + server + "\\" + share] = s;
        }

        public static void DisconnectAll()
        {
            foreach (var s in _sessions.Values) { try { s.Client?.Logoff(); s.Client?.Disconnect(); } catch { } }
            _sessions.Clear();
        }

        /// Drop the session for the share that contains uncPath (if any).
        public static bool Disconnect(string uncPath)
        {
            string root = ShareRoot(uncPath);
            if (_sessions.TryRemove(root, out var s))
            {
                try { s.Client?.Logoff(); s.Client?.Disconnect(); } catch { }
                return true;
            }
            return false;
        }

        private static void Establish(Session s)
        {
            var client = new SMB2Client();
            bool ok;
            try { ok = client.Connect(s.Server, SMBTransportType.DirectTCPTransport); }
            catch (Exception ex) { throw new IOException($"Cannot reach {s.Server}:445 — {ex.Message}"); }
            if (!ok) throw new IOException($"Cannot reach {s.Server}:445 (connection timeout)");

            NTStatus st = client.Login(s.Domain, s.User, s.Pass);
            if (st != NTStatus.STATUS_SUCCESS)
            {
                try { client.Disconnect(); } catch { }
                if (st == NTStatus.STATUS_LOGON_FAILURE || st == NTStatus.STATUS_ACCESS_DENIED)
                    throw new NasAuthException($"Login failed on {s.Server} — check username/password ({st})");
                throw new IOException($"Login failed on {s.Server}: {st}");
            }

            ISMBFileStore? store = client.TreeConnect(s.Share, out st);
            if (st != NTStatus.STATUS_SUCCESS || store == null)
            {
                try { client.Logoff(); client.Disconnect(); } catch { }
                if (st == NTStatus.STATUS_ACCESS_DENIED) throw new NasAuthException($"Access denied to \\\\{s.Server}\\{s.Share} — insufficient permissions");
                throw new IOException($"Cannot mount share \\\\{s.Server}\\{s.Share}: {st}");
            }
            s.Client = client; s.Store = store;
        }

        private static Session SessionFor(string uncPath)
        {
            if (_sessions.TryGetValue(ShareRoot(uncPath), out var s)) return s;
            throw new IOException($"No NAS session for {ShareRoot(uncPath)} — add the path via the 🌐 NAS button first.");
        }

        private static string RelPath(Session s, string uncPath)
        {
            string root = "\\\\" + s.Server + "\\" + s.Share;
            return Normalize(uncPath).Substring(root.Length).TrimStart('\\');
        }

        /// Run an op on the session's store; on stale connection reconnect once and retry.
        private static T WithStore<T>(string uncPath, Func<ISMBFileStore, Session, T> op)
        {
            var s = SessionFor(uncPath);
            lock (s.Lock)
            {
                try
                {
                    return op(s.Store ?? throw new IOException("NAS session not connected"), s);
                }
                catch (NasAuthException) { throw; }
                catch (IOException) { throw; }
                catch (Exception ex)
                {
                    // Dead socket / broken pipe → one reconnect attempt.
                    try { Establish(s); return op(s.Store!, s); }
                    catch (IOException) { throw; }
                    catch (Exception ex2) { throw new IOException($"NAS error on {s.Server}: {ex2.Message}", ex); }
                }
            }
        }

        private static NTStatus OpenFile(ISMBFileStore store, string rel, bool isDir, out object handle)
        {
            handle = null!;
            return store.CreateFile(out handle, out _,
                rel,
                AccessMask.MAXIMUM_ALLOWED,
                SMBLibrary.FileAttributes.Normal,
                ShareAccess.Read | ShareAccess.Write,
                CreateDisposition.FILE_OPEN,
                isDir ? CreateOptions.FILE_DIRECTORY_FILE : CreateOptions.FILE_NON_DIRECTORY_FILE | CreateOptions.FILE_SYNCHRONOUS_IO_NONALERT,
                null);
        }

        /// true=dir, false=file, null=doesn't exist / not accessible.
        public static bool? ExistsAsDir(string uncPath)
        {
            try
            {
                return WithStore(uncPath, (store, s) =>
                {
                    string rel = RelPath(s, uncPath);
                    if (rel.Length == 0) return (bool?)true; // share root = directory
                    NTStatus st = OpenFile(store, rel, true, out object h);
                    if (st == NTStatus.STATUS_SUCCESS) { store.CloseFile(h); return (bool?)true; }
                    st = OpenFile(store, rel, false, out h);
                    if (st == NTStatus.STATUS_SUCCESS) { store.CloseFile(h); return (bool?)false; }
                    if (st == NTStatus.STATUS_ACCESS_DENIED) throw new NasAuthException($"Access denied: {uncPath}");
                    return (bool?)null;
                });
            }
            catch (IOException) { throw; }
            catch { return null; }
        }

        public static bool IsFile(string uncPath) => ExistsAsDir(uncPath) == false;
        public static bool IsDirectory(string uncPath) => ExistsAsDir(uncPath) == true;

        public static long GetLength(string uncPath)
        {
            return WithStore(uncPath, (store, s) =>
            {
                string rel = RelPath(s, uncPath);
                NTStatus st = OpenFile(store, rel, false, out object h);
                if (st != NTStatus.STATUS_SUCCESS)
                    throw new IOException($"Cannot open {uncPath}: {st}");
                try
                {
                    st = store.GetFileInformation(out FileInformation? info, h, FileInformationClass.FileStandardInformation);
                    if (st != NTStatus.STATUS_SUCCESS || info is not FileStandardInformation fsi)
                        throw new IOException($"Cannot stat {uncPath}: {st}");
                    return fsi.EndOfFile;
                }
                finally { store.CloseFile(h); }
            });
        }

        public sealed class NasEntry
        {
            public string FullPath = "";
            public string Name = "";
            public bool IsDirectory;
            public long Size;
        }

        /// Direct children of a UNC directory (skips . and ..).
        public static List<NasEntry> Enumerate(string uncDir)
        {
            return WithStore(uncDir, (store, s) =>
            {
                string rel = RelPath(s, uncDir);
                NTStatus st = OpenFile(store, rel, true, out object h);
                if (st != NTStatus.STATUS_SUCCESS && rel.Length == 0)
                    st = OpenFile(store, ".", true, out h);   // some servers want "." for share root
                if (st != NTStatus.STATUS_SUCCESS)
                    throw new IOException($"Cannot open directory {uncDir}: {st}");
                try
                {
                    st = store.QueryDirectory(out List<QueryDirectoryFileInformation> entries, h, "*", FileInformationClass.FileDirectoryInformation);
                    if (st != NTStatus.STATUS_SUCCESS && st != NTStatus.STATUS_NO_MORE_FILES)
                        throw new IOException($"Cannot list {uncDir}: {st}");
                    var result = new List<NasEntry>();
                    string basePath = Normalize(uncDir).TrimEnd('\\');
                    foreach (var e in entries)
                    {
                        if (e is not FileDirectoryInformation fdi) continue;
                        if (fdi.FileName is "." or "..") continue;
                        bool isDir = (fdi.FileAttributes & SMBLibrary.FileAttributes.Directory) != 0;
                        result.Add(new NasEntry
                        {
                            Name = fdi.FileName,
                            FullPath = basePath + "\\" + fdi.FileName,
                            IsDirectory = isDir,
                            Size = isDir ? 0 : fdi.EndOfFile
                        });
                    }
                    return result;
                }
                finally { store.CloseFile(h); }
            });
        }

        /// Seekable read stream over an SMB file. Multiple streams can be open
        /// concurrently on one session (each owns its file handle).
        public static Stream OpenRead(string uncPath)
        {
            var s = SessionFor(uncPath);
            lock (s.Lock)
            {
                if (s.Store == null) Establish(s);
                string rel = RelPath(s, uncPath);
                NTStatus st = OpenFile(s.Store!, rel, false, out object h);
                if (st == NTStatus.STATUS_ACCESS_DENIED) throw new NasAuthException($"Access denied: {uncPath}");
                if (st != NTStatus.STATUS_SUCCESS)
                    throw new IOException($"Cannot open {uncPath}: {st}");
                return new SmbReadStream(s, h, uncPath);
            }
        }

        private sealed class SmbReadStream : Stream
        {
            private readonly Session _s;
            private object _handle;
            private readonly string _path;
            private long _pos = -1;   // lazily resolved
            private long _len = -1;
            private bool _disposed;

            public SmbReadStream(Session s, object handle, string path) { _s = s; _handle = handle; _path = path; }

            public override bool CanRead => true;
            public override bool CanSeek => true;
            public override bool CanWrite => false;
            public override long Length
            {
                get
                {
                    if (_len < 0)
                    {
                        lock (_s.Lock)
                        {
                            var st = _s.Store!.GetFileInformation(out FileInformation? info, _handle, FileInformationClass.FileStandardInformation);
                            _len = (st == NTStatus.STATUS_SUCCESS && info is FileStandardInformation fsi) ? fsi.EndOfFile : 0;
                        }
                    }
                    return _len;
                }
            }
            public override long Position { get => _pos < 0 ? 0 : _pos; set => _pos = value; }
            public override long Seek(long offset, SeekOrigin origin)
            {
                long basePos = origin == SeekOrigin.Begin ? 0 : origin == SeekOrigin.Current ? Position : Length;
                _pos = basePos + offset;
                if (_pos < 0) _pos = 0;
                return _pos;
            }

            public override int Read(byte[] buffer, int offset, int count)
            {
                if (_disposed) throw new ObjectDisposedException(nameof(SmbReadStream));
                lock (_s.Lock)
                {
                    EnsureFresh();
                    int total = 0;
                    while (total < count)
                    {
                        int want = Math.Min(count - total, 1024 * 1024); // 1MB per SMB read
                        var st = _s.Store!.ReadFile(out byte[]? data, _handle, _pos, want);
                        if (st == NTStatus.STATUS_END_OF_FILE || data == null || data.Length == 0) break;
                        if (st != NTStatus.STATUS_SUCCESS) break;
                        Buffer.BlockCopy(data, 0, buffer, offset + total, data.Length);
                        _pos += data.Length;
                        total += data.Length;
                    }
                    return total;
                }
            }

            public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken)
                => Task.Run(() => Read(buffer, offset, count), cancellationToken);

            private void EnsureFresh()
            {
                if (_pos < 0) _pos = 0;
                if (_s.Store == null)
                {
                    Establish(_s);
                    string rel = RelPath(_s, _path);
                    NTStatus st = OpenFile(_s.Store!, rel, false, out object h);
                    if (st != NTStatus.STATUS_SUCCESS) throw new IOException($"Cannot reopen {_path}: {st}");
                    _handle = h;
                }
            }

            public override void Flush() { }
            public override void SetLength(long value) => throw new NotSupportedException();
            public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

            protected override void Dispose(bool disposing)
            {
                if (!_disposed)
                {
                    _disposed = true;
                    lock (_s.Lock) { try { _s.Store?.CloseFile(_handle); } catch { } }
                }
                base.Dispose(disposing);
            }
        }
    }

    /// <summary>
    /// Uniform file access for the upload queue: UNC paths go through the
    /// NAS session (SMBLibrary) when one exists — otherwise System.IO so
    /// already-authenticated Windows shares keep working; local paths always
    /// use System.IO.
    /// </summary>
    public static class LocalIo
    {
        public static bool IsUnc(string? path) => NasManager.IsUncPath(path);

        /// File exists? (either kind)
        public static bool FileExists(string path)
            => IsUnc(path) && NasManager.HasSession(path) ? NasManager.IsFile(path) : File.Exists(path);

        public static bool DirExists(string path)
            => IsUnc(path) && NasManager.HasSession(path) ? NasManager.IsDirectory(path) : Directory.Exists(path);

        public static long GetLength(string path)
        {
            if (IsUnc(path) && NasManager.HasSession(path)) return NasManager.GetLength(path);
            return new FileInfo(path).Length;
        }

        public static string GetName(string path)
        {
            if (IsUnc(path)) return NasManager.Normalize(path).TrimEnd('\\').Split('\\')[^1];
            return Path.GetFileName(path.TrimEnd('\\', '/'));
        }

        public static Stream OpenRead(string path, bool randomAccess = false)
        {
            if (IsUnc(path) && NasManager.HasSession(path)) return NasManager.OpenRead(path);
            var opts = randomAccess ? FileOptions.RandomAccess : FileOptions.SequentialScan;
            return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024, opts);
        }

        public sealed class Entry
        {
            public string FullPath = "";
            public string Name = "";
            public bool IsDirectory;
            public long Size;
        }

        /// Direct children of a local or NAS directory.
        public static List<Entry> Enumerate(string dir)
        {
            if (IsUnc(dir) && NasManager.HasSession(dir))
            {
                var l = new List<Entry>();
                foreach (var e in NasManager.Enumerate(dir))
                    l.Add(new Entry { FullPath = e.FullPath, Name = e.Name, IsDirectory = e.IsDirectory, Size = e.Size });
                return l;
            }
            var result = new List<Entry>();
            var di = new DirectoryInfo(dir);
            foreach (var f in di.GetFiles()) result.Add(new Entry { FullPath = f.FullName, Name = f.Name, IsDirectory = false, Size = f.Length });
            foreach (var d in di.GetDirectories()) result.Add(new Entry { FullPath = d.FullName, Name = d.Name, IsDirectory = true, Size = 0 });
            return result;
        }
    }
}
