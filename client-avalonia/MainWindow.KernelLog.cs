using System;
using System.IO;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace PS5Upload
{
    // Kernel log viewer (sysctl kern.msgbuf).
    public partial class MainWindow
    {
        private DispatcherTimer? _klogTimer;
        private bool _klogBusy;

        private int KlogTailBytes => KlogTailSelect.SelectedIndex switch
        {
            0 => 32 * 1024,
            1 => 128 * 1024,
            _ => 0            // full buffer
        };

        private async void KlogRefresh_Click(object? sender, RoutedEventArgs e)
        {
            if (_klogBusy) return;
            if (!_protocol.IsConnected) { KlogStatus.Text = "Not connected"; return; }
            _klogBusy = true;
            KlogStatus.Text = "Reading kern.msgbuf…";
            var txt = await _protocol.GetKernelLogAsync(KlogTailBytes);
            _klogBusy = false;
            if (txt == null)
            {
                KlogStatus.Text = $"❌ {_protocol.LastError}";
                return;
            }
            KlogText.Text = txt;
            if (KlogAutoScroll.IsChecked == true)
                KlogText.CaretIndex = txt.Length;
            KlogStatus.Text = $"{txt.Length:N0} bytes · {DateTime.Now:HH:mm:ss}";
        }

        private void KlogAutoRefresh_Changed(object? sender, RoutedEventArgs e)
        {
            if (KlogAutoRefresh.IsChecked == true)
            {
                _klogTimer ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
                _klogTimer.Tick -= KlogTimer_Tick;
                _klogTimer.Tick += KlogTimer_Tick;
                _klogTimer.Start();
                KlogRefresh_Click(sender, e);
            }
            else
            {
                _klogTimer?.Stop();
            }
        }

        private async void KlogTimer_Tick(object? sender, EventArgs e)
        {
            if (_klogBusy || !_protocol.IsConnected) return;
            _klogBusy = true;
            var txt = await _protocol.GetKernelLogAsync(KlogTailBytes);
            _klogBusy = false;
            if (txt != null)
            {
                KlogText.Text = txt;
                if (KlogAutoScroll.IsChecked == true)
                    KlogText.CaretIndex = txt.Length;
                KlogStatus.Text = $"{txt.Length:N0} bytes · {DateTime.Now:HH:mm:ss} (auto)";
            }
        }

        private async void KlogCopy_Click(object? sender, RoutedEventArgs e)
        {
            var cb = TopLevel.GetTopLevel(this)?.Clipboard;
            if (cb != null && !string.IsNullOrEmpty(KlogText.Text))
            {
                var item = new Avalonia.Input.DataTransferItem();
                item.Set(Avalonia.Input.DataFormat.Text, KlogText.Text);
                var data = new Avalonia.Input.DataTransfer();
                data.Add(item);
                await cb.SetDataAsync(data);
                KlogStatus.Text = "Copied to clipboard";
            }
        }

        private async void KlogSave_Click(object? sender, RoutedEventArgs e)
        {
            if (string.IsNullOrEmpty(KlogText.Text)) { KlogStatus.Text = "Nothing to save — refresh first"; return; }
            var top = TopLevel.GetTopLevel(this);
            if (top == null) return;
            var file = await top.StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
            {
                Title = "Save kernel log",
                SuggestedFileName = $"klog_{DateTime.Now:yyyyMMdd_HHmmss}.txt"
            });
            var dest = file?.TryGetLocalPath();
            if (dest == null) return;
            try
            {
                await File.WriteAllTextAsync(dest, KlogText.Text);
                KlogStatus.Text = $"Saved to {Path.GetFileName(dest)}";
            }
            catch (Exception ex) { KlogStatus.Text = $"Save failed: {ex.Message}"; }
        }

    }
}
