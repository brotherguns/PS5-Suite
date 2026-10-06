using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Threading;

namespace PS5Upload
{
    // App Manager v2 — real per-app data from sceKernelGetAppInfo +
    // suspend/resume/kill/coredump through LncUtil (daemon IPC, needs etaHEN).
    public partial class MainWindow
    {
        private class AppEntry
        {
            public int Pid;
            public uint AppId;
            public string TitleId = "";
            public string Name = "";
            public uint AppType;
            public double CpuPct;
            public int Suspended;   // -1 unknown, 0 running, 1 suspended
            public string Display =>
                $"pid={Pid,-6} app={AppId,-4} {TitleId,-10} {Name,-20} cpu={CpuPct,6:0.00}%  " +
                (Suspended < 0 ? "?" : Suspended == 1 ? "suspended" : "running");
        }

        private readonly List<AppEntry> _apps = new();
        private AppEntry? _appSel;
        private DispatcherTimer? _appTimer;

        private async Task AppMgrLoadAsync()
        {
            var txt = await _protocol.AppListV2Async();
            if (txt == null)
            {
                AppMgrStatus.Text = $"app list failed: {_protocol.LastError}";
                return;
            }
            _apps.Clear();
            foreach (var line in txt.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                if (line.StartsWith("appinfo="))
                {
                    if (!line.EndsWith("ok")) AppMgrStatus.Text = "sceKernelGetAppInfo unavailable — app fields empty";
                    continue;
                }
                var f = line.Split('|');
                if (f.Length < 7) continue;
                _apps.Add(new AppEntry
                {
                    Pid = int.Parse(f[0]),
                    AppId = uint.Parse(f[1]),
                    TitleId = f[2],
                    Name = f[3],
                    AppType = uint.Parse(f[4]),
                    CpuPct = uint.Parse(f[5]) / 100.0,
                    Suspended = int.Parse(f[6]),
                });
            }
            AppMgrList.ItemsSource = _apps.Select(a => a.Display).ToList();
            if (!(AppMgrStatus.Text ?? "").StartsWith("sceKernelGetAppInfo"))
                AppMgrStatus.Text = $"{_apps.Count} app(s) — select one";
        }

        private async void AppMgrRefresh_Click(object? sender, RoutedEventArgs e)
        {
            if (!_protocol.IsConnected) { AppMgrStatus.Text = "Not connected"; return; }
            await AppMgrLoadAsync();
        }

        private void AppMgrAutoRefresh_Changed(object? sender, RoutedEventArgs e)
        {
            if (AppMgrAutoRefresh.IsChecked == true)
            {
                _appTimer ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
                _appTimer.Tick -= AppTimer_Tick;
                _appTimer.Tick += AppTimer_Tick;
                _appTimer.Start();
            }
            else _appTimer?.Stop();
        }

        private async void AppTimer_Tick(object? sender, EventArgs e)
        {
            if (_protocol.IsConnected) await AppMgrLoadAsync();
        }

        private void AppMgrList_SelectionChanged(object? sender, SelectionChangedEventArgs e)
        {
            var s = AppMgrList.SelectedItem?.ToString();
            _appSel = s == null ? null : _apps.FirstOrDefault(a => a.Display == s);
        }

        private async Task AppAction(Func<int, int, Task<(bool ok, string msg)>> fn)
        {
            if (_appSel == null) { AppMgrStatus.Text = "Select an app first"; return; }
            var (ok, msg) = await fn((int)_appSel.AppId, _appSel.Pid);
            AppMgrStatus.Text = ok ? $"✅ {msg}" : $"❌ {msg}";
            await AppMgrLoadAsync();
        }

        private async void AppMgrSuspend_Click(object? sender, RoutedEventArgs e)
            => await AppAction(_protocol.AppSuspendAsync);
        private async void AppMgrResume_Click(object? sender, RoutedEventArgs e)
            => await AppAction(_protocol.AppResumeAsync);
        private async void AppMgrKill_Click(object? sender, RoutedEventArgs e)
            => await AppAction(_protocol.AppKillAsync);
        private async void AppMgrCoredump_Click(object? sender, RoutedEventArgs e)
        {
            if (_appSel == null) { AppMgrStatus.Text = "Select an app first"; return; }
            var (ok, msg) = await _protocol.AppCoredumpAsync((int)_appSel.AppId);
            AppMgrStatus.Text = ok ? $"✅ {msg}" : $"❌ {msg}";
        }
    }
}
