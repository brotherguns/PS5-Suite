using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Interactivity;

namespace PS5Upload
{
    // Memory editor — ptrace/PT_IO read-write-search on PS5 processes.
    public partial class MainWindow
    {
        private List<(int pid, string name)> _memProcs = new();
        private readonly List<ulong> _memResults = new();
        private int _memPid = -1;
        private int _memPatternLen = 4;
        private bool _memScanning;

        // ---------- process list ----------

        private async void MemRefreshProcesses_Click(object? sender, RoutedEventArgs e)
        {
            if (!_protocol.IsConnected) { MemStatus.Text = "Not connected"; return; }
            MemStatus.Text = "Loading processes...";
            _memProcs = await _protocol.GetProcessListAsync();
            ApplyMemProcFilter();
            MemStatus.Text = _memProcs.Count > 0
                ? $"{_memProcs.Count} processes — select one"
                : $"Process list failed: {_protocol.LastError}";
        }

        private void MemProcessFilter_KeyUp(object? sender, Avalonia.Input.KeyEventArgs e) => ApplyMemProcFilter();

        private void ApplyMemProcFilter()
        {
            var f = (MemProcessFilter.Text ?? "").Trim();
            MemProcessList.ItemsSource = _memProcs
                .Where(p => f.Length == 0 || p.name.Contains(f, StringComparison.OrdinalIgnoreCase) || p.pid.ToString().Contains(f))
                .Select(p => $"{p.pid,6}  {p.name}")
                .ToList();
        }

        private async void MemProcessList_SelectionChanged(object? sender, SelectionChangedEventArgs e)
        {
            var s = MemProcessList.SelectedItem?.ToString();
            if (s == null) return;
            var first = s.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "";
            if (!int.TryParse(first, out int pid)) return;

            _memPid = pid;
            _memResults.Clear();
            MemResultsList.ItemsSource = null;
            MemNextScanButton.IsEnabled = false;
            MemStatus.Text = $"Loading regions for pid {pid}...";

            var txt = await _protocol.MemRegionsAsync(pid);
            if (txt == null) { MemStatus.Text = $"vmmap failed: {_protocol.LastError}"; return; }
            var regions = txt.Split('\n', StringSplitOptions.RemoveEmptyEntries).ToList();
            MemRegionsList.ItemsSource = regions;
            MemStatus.Text = $"pid {pid}: {regions.Count} regions — pick one or scan a range";
        }

        private void MemRegionsList_SelectionChanged(object? sender, SelectionChangedEventArgs e)
        {
            var s = MemRegionsList.SelectedItem?.ToString();
            if (s == null) return;
            // "start-end|prot|size|path"
            var range = s.Split('|')[0].Split('-');
            if (range.Length == 2)
            {
                MemScanStart.Text = "0x" + range[0];
                MemScanEnd.Text = "0x" + range[1];
            }
        }

        // ---------- search ----------

        private byte[]? EncodeSearchValue()
        {
            var type = (MemValueType.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "u32";
            var v = (MemValueInput.Text ?? "").Trim();
            try
            {
                switch (type)
                {
                    case "u8":    return new[] { ParseMemNumber<byte>(v, byte.TryParse) };
                    case "u16":   return BitConverter.GetBytes(ParseMemNumber<ushort>(v, ushort.TryParse));
                    case "u32":   return BitConverter.GetBytes(ParseMemNumber<uint>(v, uint.TryParse));
                    case "u64":   return BitConverter.GetBytes(ParseMemNumber<ulong>(v, ulong.TryParse));
                    case "float": return BitConverter.GetBytes(float.Parse(v, CultureInfo.InvariantCulture));
                    case "bytes": return Convert.FromHexString(v.Replace(" ", "").Replace("0x", ""));
                    default:      return null;
                }
            }
            catch { return null; }
        }

        private delegate bool TryNum<T>(string s, NumberStyles st, IFormatProvider fp, out T val);
        private static T ParseMemNumber<T>(string v, TryNum<T> tryParse) where T : struct
        {
            v = v.Trim();
            if (v.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                if (tryParse(v[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out T r)) return r;
            }
            if (tryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out T d)) return d;
            if (tryParse(v, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out T h)) return h;
            throw new FormatException(v);
        }

        private static ulong ParseMemAddr(string? text)
        {
            var v = (text ?? "").Trim();
            if (v.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) v = v[2..];
            if (ulong.TryParse(v, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out ulong hex)) return hex;
            return ulong.TryParse(v, out ulong dec) ? dec : 0;
        }

        private async void MemFirstScan_Click(object? sender, RoutedEventArgs e)
        {
            if (_memScanning) return;
            if (_memPid <= 0) { MemStatus.Text = "Select a process first"; return; }
            var pattern = EncodeSearchValue();
            if (pattern == null || pattern.Length == 0) { MemStatus.Text = "Invalid value — check type/input"; return; }

            ulong start = ParseMemAddr(MemScanStart.Text), end = ParseMemAddr(MemScanEnd.Text);
            if (end <= start) { MemStatus.Text = "Bad range — set start/end or pick a region"; return; }

            _memPatternLen = pattern.Length;
            _memScanning = true;
            MemFirstScanButton.IsEnabled = false;
            MemStatus.Text = $"Scanning 0x{start:X}–0x{end:X} ({(end - start) / 1024 / 1024}MB)…";

            var txt = await _protocol.MemSearchAsync(_memPid, start, end, pattern);
            _memScanning = false;
            MemFirstScanButton.IsEnabled = true;

            if (txt == null) { MemStatus.Text = $"Scan failed: {_protocol.LastError}"; return; }
            var lines = txt.Split('\n', StringSplitOptions.RemoveEmptyEntries);
            _memResults.Clear();
            bool truncated = false, timedOut = false;
            foreach (var l in lines)
            {
                if (l == "TRUNCATED") { truncated = true; continue; }
                if (l == "TIMEOUT") { timedOut = true; continue; }
                if (l == "NO_MATCHES") break;
                var a = ParseMemAddr(l);
                if (a != 0) _memResults.Add(a);
            }
            MemResultsList.ItemsSource = _memResults.Select(a => $"0x{a:X}").ToList();
            MemStatus.Text = $"{_memResults.Count} match(es)" +
                (truncated ? " — hit 1024 cap, narrow with Next Scan" : "") +
                (timedOut ? " — scan timed out, partial results" : "");
            MemNextScanButton.IsEnabled = _memResults.Count > 0;
        }

        // Next Scan = client-side re-read of each candidate against the new value
        private async void MemNextScan_Click(object? sender, RoutedEventArgs e)
        {
            if (_memScanning || _memResults.Count == 0) return;
            var pattern = EncodeSearchValue();
            if (pattern == null || pattern.Length != _memPatternLen)
            {
                MemStatus.Text = $"Next Scan needs the same type ({_memPatternLen} bytes)";
                return;
            }

            _memScanning = true;
            MemNextScanButton.IsEnabled = false;
            var kept = new List<ulong>();
            for (int i = 0; i < _memResults.Count; i++)
            {
                var bytes = await _protocol.MemReadAsync(_memPid, _memResults[i], pattern.Length);
                if (bytes != null && bytes.Length >= pattern.Length && bytes.AsSpan(0, pattern.Length).SequenceEqual(pattern))
                    kept.Add(_memResults[i]);
                if (i % 64 == 0) MemStatus.Text = $"Re-checking {i}/{_memResults.Count}…";
            }
            _memResults.Clear();
            _memResults.AddRange(kept);
            MemResultsList.ItemsSource = _memResults.Select(a => $"0x{a:X}").ToList();
            MemStatus.Text = $"{_memResults.Count} match(es) after filter";
            _memScanning = false;
            MemNextScanButton.IsEnabled = _memResults.Count > 0;
        }

        // ---------- read / write ----------

        private void MemResultsList_SelectionChanged(object? sender, SelectionChangedEventArgs e)
        {
            var s = MemResultsList.SelectedItem?.ToString();
            if (s == null) return;
            MemAddrInput.Text = s;
            MemRead_Click(sender, e);
        }

        private async void MemRead_Click(object? sender, RoutedEventArgs e)
        {
            if (_memPid <= 0) { MemStatus.Text = "Select a process first"; return; }
            ulong addr = ParseMemAddr(MemAddrInput.Text);
            if (addr == 0) { MemStatus.Text = "Enter an address (0x…)"; return; }
            var data = await _protocol.MemReadAsync(_memPid, addr, 256);
            if (data == null) { MemStatus.Text = $"Read failed: {_protocol.LastError}"; return; }
            MemDumpText.Text = MemHexDump(data, addr);
            MemStatus.Text = $"Read {data.Length}B @ 0x{addr:X}";
        }

        private async void MemWrite_Click(object? sender, RoutedEventArgs e)
        {
            if (_memPid <= 0) { MemStatus.Text = "Select a process first"; return; }
            ulong addr = ParseMemAddr(MemAddrInput.Text);
            if (addr == 0) { MemStatus.Text = "Enter an address (0x…)"; return; }
            byte[] bytes;
            try { bytes = Convert.FromHexString((MemWriteHex.Text ?? "").Replace(" ", "").Replace("0x", "")); }
            catch { MemStatus.Text = "Invalid hex data"; return; }
            if (bytes.Length == 0) { MemStatus.Text = "Nothing to write"; return; }

            var (ok, msg) = await _protocol.MemWriteAsync(_memPid, addr, bytes);
            MemStatus.Text = ok ? $"✅ {msg}" : $"❌ {msg}";
            if (ok) MemRead_Click(sender, e);   // refresh the dump
        }

        private static string MemHexDump(byte[] data, ulong baseAddr)
        {
            var sb = new StringBuilder();
            for (int i = 0; i < data.Length; i += 16)
            {
                sb.Append($"{baseAddr + (ulong)i:X8}  ");
                for (int j = 0; j < 16; j++)
                    sb.Append(i + j < data.Length ? $"{data[i + j]:X2} " : "   ");
                sb.Append(' ');
                for (int j = 0; j < 16 && i + j < data.Length; j++)
                {
                    byte b = data[i + j];
                    sb.Append(b >= 32 && b < 127 ? (char)b : '.');
                }
                sb.AppendLine();
            }
            return sb.ToString();
        }
    }
}
