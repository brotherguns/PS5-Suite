using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace PS5Upload
{
    public partial class MainWindow
    {
        // ============================================================
        // SETTINGS
        // ============================================================
        private void LoadSettings()
        {
            try
            {
                // Renamed with the app (PS5 Suite); fall back to the legacy file.
                const string settingsFile = "ps5suite_settings.json";
                string effective = File.Exists(settingsFile) ? settingsFile
                    : File.Exists("ps5upload_settings.json") ? "ps5upload_settings.json" : settingsFile;
                if (File.Exists(effective))
                {
                    string json = File.ReadAllText(effective);
                    var settings = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, object>>(json);
                    if (settings != null)
                    {
                        if (settings.ContainsKey("AutoSendPayload"))
                            _autoSendPayload = settings["AutoSendPayload"].ToString() == "True";
                        if (settings.ContainsKey("PayloadPath"))
                            _payloadPath = settings["PayloadPath"].ToString() ?? "";
                        if (settings.ContainsKey("PayloadPort"))
                            int.TryParse(settings["PayloadPort"].ToString(), out _payloadPort);
                    }
                }
            }
            catch (Exception ex) { Log($"⚠️ Failed to load settings: {ex.Message}"); }
        }

        private void SaveSettings()
        {
            try
            {
                const string settingsFile = "ps5suite_settings.json";
                var settings = new Dictionary<string, object>
                {
                    ["AutoSendPayload"] = _autoSendPayload,
                    ["PayloadPath"] = _payloadPath,
                    ["PayloadPort"] = _payloadPort
                };
                string json = System.Text.Json.JsonSerializer.Serialize(settings, new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
                File.WriteAllText(settingsFile, json);
            }
            catch (Exception ex) { Log($"⚠️ Failed to save settings: {ex.Message}"); }
        }

        // ============================================================
        // CONNECTION
        // ============================================================
        private async Task AutoSendPayloadOnStartup()
        {
            await Task.Delay(1000);
            if (string.IsNullOrEmpty(_ps5IpAddress)) { Log("⚠️ Auto-send payload: No PS5 IP address configured"); return; }
            if (string.IsNullOrEmpty(_payloadPath) || !File.Exists(_payloadPath)) { Log("⚠️ Auto-send payload: Payload file not found"); return; }

            Log($"📤 Auto-sending payload to {_ps5IpAddress}:{_payloadPort}...");
            var progress = new InlineProgress<long>(bytes => { });
            bool success = await PS5Protocol.SendPayloadAsync(_ps5IpAddress, _payloadPath, _payloadPort, progress);
            if (success)
            {
                Log($"✅ Payload sent successfully ({new FileInfo(_payloadPath).Length} bytes)");
                await Task.Delay(2000);
                await ConnectToPS5Async();
            }
            else
                Log("❌ Failed to send payload");
        }

        private async Task ConnectToPS5Async()
        {
            if (await _protocol.ConnectAsync(_ps5IpAddress))
            {
                Log("✅ Connected to PS5 successfully");
                await Dispatcher.UIThread.InvokeAsync(() =>
                {
                    ConnectButton.Content = "🟢 Disconnect";
                    ConnectButton.Background = new SolidColorBrush(Colors.Green);
                    UploadButton.IsEnabled = true;
                    MountGamesButton.IsEnabled = true;
                });
                await LoadPS5DirectoryAsync(_currentPS5Path);
            }
            else
                Log("❌ Failed to connect to PS5");
        }

        private CancellationTokenSource? _connectCts;
        private bool _connecting;

        private async void AutoDetectButton_Click(object? sender, RoutedEventArgs e)
        {
            Log("🔍 Auto-discovering PS5 on the network...");
            AutoDetectButton.IsEnabled = false;
            try
            {
                var found = await PS5Protocol.DiscoverPS5Async(3000);
                if (found == null)
                {
                    Log("❌ No PS5 answered — is the payload running?");
                    await ShowMessageAsync("No PS5 found on the network.\nMake sure the payload is running.", "Auto-Discovery");
                    return;
                }
                IpAddressTextBox.Text = found.Value.ip;
                Log($"🔍 Found PS5 at {found.Value.ip}:{found.Value.port}");
            }
            catch (Exception ex) { Log($"❌ Discovery failed: {ex.Message}"); }
            finally { AutoDetectButton.IsEnabled = true; }
        }

        private async void ConnectButton_Click(object? sender, RoutedEventArgs e)
        {
            // While a connect attempt is running the button becomes Cancel
            if (_connecting)
            {
                Log("⏹ Connect cancelled by user");
                _connectCts?.Cancel();
                return;
            }

            string ipAddress = IpAddressTextBox.Text?.Trim() ?? "";

            // Empty IP => auto-discover: broadcast a UDP probe; the running
            // payload answers with its IP + actual bound port.
            if (string.IsNullOrEmpty(ipAddress))
            {
                Log("🔍 Auto-discovering PS5 on the network...");
                try
                {
                    var found = await PS5Protocol.DiscoverPS5Async(2500);
                    if (found == null)
                    {
                        Log("❌ No PS5 answered the discovery probe");
                        await ShowMessageAsync("No PS5 found on the network.\nMake sure the payload is running, or enter the IP manually.", "Auto-Discovery");
                        return;
                    }
                    ipAddress = found.Value.ip;
                    Log($"🔍 Found PS5 at {ipAddress}:{found.Value.port}");
                    await Dispatcher.UIThread.InvokeAsync(() => IpAddressTextBox.Text = ipAddress);
                }
                catch (Exception ex)
                {
                    Log($"❌ Discovery failed: {ex.Message}");
                    await ShowMessageAsync($"Discovery failed:\n{ex.Message}", "Auto-Discovery");
                    return;
                }
            }

            _ps5IpAddress = ipAddress;

            if (_protocol.IsConnected)
            {
                ConnectButton.IsEnabled = false;
                Log("🔌 Disconnecting from PS5...");
                Log("🔌 Disconnecting from PS5...");
                StopHwAutoRefresh();
                _protocol.Disconnect();
                _shellActive = false;
                _shellCurrentDir = "/data";

                await Dispatcher.UIThread.InvokeAsync(() =>
                {
                    ConnectButton.Content = "🔴 Disconnected";
                    ConnectButton.Background = new SolidColorBrush(Colors.DarkRed);
                    UploadButton.IsEnabled = false;
                    MountGamesButton.IsEnabled = false;
                    _ps5Files.Clear();
                });
                Log("✅ Disconnected from PS5");

                await Task.Delay(1000);
                await Dispatcher.UIThread.InvokeAsync(() =>
                {
                    ConnectButton.Content = "🔵 Connect";
                    ConnectButton.Background = new SolidColorBrush(Color.FromRgb(0, 122, 204));
                });
                ConnectButton.IsEnabled = true;
            }
            else
            {
                Log($"Connecting to PS5 at {ipAddress}...");
                _connecting = true;
                _connectCts = new CancellationTokenSource();
                await Dispatcher.UIThread.InvokeAsync(() =>
                {
                    ConnectButton.Content = "⏹ Cancel";
                    ConnectButton.Background = new SolidColorBrush(Colors.DarkOrange);
                });
                try
                {
                    if (await _protocol.ConnectAsync(ipAddress, 9113, _connectCts.Token))
                    {
                        Log("✅ Connected to PS5 successfully");
                        await Dispatcher.UIThread.InvokeAsync(() =>
                        {
                            ConnectButton.Content = "🟢 Disconnect";
                            ConnectButton.Background = new SolidColorBrush(Colors.DarkGreen);
                            UploadButton.IsEnabled = true;
                            MountGamesButton.IsEnabled = true;
                        });
                        await OpenShellAsync();
                        await LoadPS5DirectoryAsync(_currentPS5Path);
                        await RefreshStorageInfoAsync();
                    }
                    else
                    {
                        Log("❌ Failed to connect to PS5");
                        await ShowMessageAsync("Failed to connect to PS5. Make sure the payload is running.", "Connection Error");
                    }
                }
                catch (OperationCanceledException)
                {
                    Log("⏹ Connect cancelled");
                }
                finally
                {
                    _connecting = false;
                    _connectCts?.Dispose();
                    _connectCts = null;
                    bool stillConnected = _protocol.IsConnected;
                    await Dispatcher.UIThread.InvokeAsync(() =>
                    {
                        if (!stillConnected)
                        {
                            ConnectButton.Content = "🔵 Connect";
                            ConnectButton.Background = new SolidColorBrush(Color.FromRgb(0, 122, 204));
                        }
                    });
                }
            }
        }

        // ============================================================
        // PS5 DIRECTORY BROWSING
        // ============================================================
        private async Task LoadPS5DirectoryAsync(string path)
        {
            try
            {
                if (!_protocol.IsConnected)
                {
                    Log($"⚠️ Connection lost (LastError: {_protocol.LastError}), reconnecting...");
                    if (!await _protocol.ConnectAsync(_ps5IpAddress))
                    {
                        await ShowMessageAsync("Failed to reconnect to PS5", "Connection Error");
                        return;
                    }
                    Log("✅ Reconnected successfully");
                }
                var sw = System.Diagnostics.Stopwatch.StartNew();
                _currentPS5Path = path;
                CurrentPathTextBox.Text = path;

                var entries = await _protocol.ListDirAsync(path);
                Log($"🔍 DEBUG: ListDir returned {entries.Count()} entries for path: {path}");
                foreach (var entry in entries.Take(5))
                    Log($"  - {(entry.IsDirectory ? "📁" : "📄")} {entry.Name} ({entry.Size} bytes)");
                if (entries.Count() > 5)
                    Log($"  ... and {entries.Count() - 5} more");

                var items = new List<PS5FileItem>();
                if (path != "/")
                {
                    string? parentPath = Path.GetDirectoryName(path);
                    if (string.IsNullOrEmpty(parentPath)) parentPath = "/";
                    else parentPath = parentPath.Replace("\\", "/");
                    items.Add(new PS5FileItem { Name = "..", FullPath = parentPath, Icon = "📁", IsDirectory = true, Size = 0 });
                }

                foreach (var entry in entries)
                {
                    items.Add(new PS5FileItem
                    {
                        Name = entry.Name,
                        IsDirectory = entry.IsDirectory,
                        Size = entry.Size,
                        FullPath = $"{path}/{entry.Name}".Replace("//", "/"),
                        Icon = entry.IsDirectory ? "📁" : "📄"
                    });
                }

                var sortedItems = items.OrderBy(i => i.IsDirectory ? 0 : 1).ThenBy(i => i.Name, StringComparer.OrdinalIgnoreCase).ToList();

                _ps5Files.Clear();
                foreach (var item in sortedItems) _ps5Files.Add(item);

                var t4 = sw.ElapsedMilliseconds;
                Log("📂 Loaded " + entries.Count().ToString() + " items (Total: " + t4.ToString() + "ms)");
                ApplySearchFilter();
            }
            catch (Exception ex)
            {
                if (_protocol.IsConnected)
                {
                    Log($"❌ Failed to load directory: {ex.Message}");
                    await ShowMessageAsync($"Failed to load directory: {ex.Message}", "Error");
                }
            }
        }

        private void ApplySearchFilter()
        {
            _ps5FilesFiltered.Clear();
            if (string.IsNullOrWhiteSpace(_searchQuery))
            {
                foreach (var file in _ps5Files) _ps5FilesFiltered.Add(file);
            }
            else
            {
                string query = _searchQuery.ToLower();
                foreach (var file in _ps5Files)
                    if (file.Name.ToLower().Contains(query))
                        _ps5FilesFiltered.Add(file);
            }
        }

        // ============================================================
        // LOCAL FILES
        // ============================================================
        private async void BrowseFilesButton_Click(object? sender, RoutedEventArgs e)
        {
            var topLevel = TopLevel.GetTopLevel(this);
            if (topLevel == null) return;
            var files = await topLevel.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { Title = "Select files to upload", AllowMultiple = true });
            foreach (var file in files)
            {
                var path = file.TryGetLocalPath();
                if (path != null)
                {
                    FileInfo info = new FileInfo(path);
                    
                    // Check if it's a ZIP file - extract and add contents
                    if (info.Extension.Equals(".zip", StringComparison.OrdinalIgnoreCase))
                    {
                        await ExtractAndAddZipContents(path, info.Name);
                    }
                    else
                    {
                        _localFiles.Add(new LocalFileItem { Name = info.Name, FullPath = path, Icon = "📄", IsDirectory = false, Size = info.Length });
                    }
                }
            }
        }

        private async void BrowseFolderButton_Click(object? sender, RoutedEventArgs e)
        {
            var topLevel = TopLevel.GetTopLevel(this);
            if (topLevel == null) return;
            var folders = await topLevel.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Select folder to upload", AllowMultiple = false });
            if (folders.Count > 0)
            {
                var path = folders[0].TryGetLocalPath();
                if (path != null)
                {
                    DirectoryInfo dirInfo = new DirectoryInfo(path);
                    _localFiles.Add(new LocalFileItem { Name = dirInfo.Name, FullPath = path, Icon = "📁", IsDirectory = true, Size = 0 });
                }
            }
        }

        private void ClearLocalFiles_Click(object? sender, RoutedEventArgs e) => _localFiles.Clear();

        private void LocalFilesListBox_DragOver(object? sender, DragEventArgs e) => e.DragEffects = DragDropEffects.Copy;

        private void LocalFilesListBox_Drop(object? sender, DragEventArgs e)
        {
            // Avalonia 12: Use TryGetFiles() on DataTransfer
            if (e.DataTransfer.TryGetFiles() is { } files)
            {
                foreach (var item in files)
                {
                    var path = item.Path.LocalPath;
                    if (path == null) continue;
                    
                    if (File.Exists(path))
                    {
                        FileInfo info = new FileInfo(path);
                        
                        // Check if it's a ZIP file - extract and add contents
                        if (info.Extension.Equals(".zip", StringComparison.OrdinalIgnoreCase))
                        {
                            _ = ExtractAndAddZipContents(path, info.Name);
                        }
                        else
                        {
                            _localFiles.Add(new LocalFileItem { Name = info.Name, FullPath = path, Icon = "📄", IsDirectory = false, Size = info.Length });
                        }
                    }
                    else if (Directory.Exists(path))
                    {
                        DirectoryInfo dirInfo = new DirectoryInfo(path);
                        _localFiles.Add(new LocalFileItem { Name = dirInfo.Name, FullPath = path, Icon = "📁", IsDirectory = true, Size = 0 });
                    }
                }
            }
        }

        // ============================================================
        // ZIP ARCHIVE SUPPORT (ps5upload-style)
        // Extracts .zip files and adds contents to upload queue
        // ============================================================
        private async Task ExtractAndAddZipContents(string zipPath, string zipName)
        {
            try
            {
                Log($"📦 Extracting ZIP: {zipName}");
                
                // Create temp extraction directory
                string tempDir = Path.Combine(Path.GetTempPath(), "ps5suite_" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(tempDir);
                
                // Extract ZIP
                await Task.Run(() => System.IO.Compression.ZipFile.ExtractToDirectory(zipPath, tempDir));
                
                // Add all extracted files
                var files = Directory.GetFiles(tempDir, "*", SearchOption.AllDirectories);
                var dirs = Directory.GetDirectories(tempDir, "*", SearchOption.AllDirectories);
                
                Log($"📦 Found {files.Length} files, {dirs.Length} folders in ZIP");
                
                // Add root directory as a folder item
                string zipBaseName = Path.GetFileNameWithoutExtension(zipName);
                _localFiles.Add(new LocalFileItem { 
                    Name = zipBaseName, 
                    FullPath = tempDir, 
                    Icon = "📁", 
                    IsDirectory = true, 
                    Size = 0 
                });
                
                Log($"✅ ZIP extracted to temp folder - added '{zipBaseName}' to upload queue");
                Log($"⚠️ Will upload extracted contents, not the .zip file");
            }
            catch (Exception ex)
            {
                Log($"❌ Failed to extract ZIP: {ex.Message}");
                // Fall back to adding the zip file itself
                FileInfo info = new FileInfo(zipPath);
                _localFiles.Add(new LocalFileItem { Name = info.Name, FullPath = zipPath, Icon = "📄", IsDirectory = false, Size = info.Length });
            }
        }

        // ============================================================
        // PS5 FILE OPERATIONS
        // ============================================================
        private void PS5FilesListBox_DoubleTapped(object? sender, TappedEventArgs e)
        {
            if (PS5FilesListBox.SelectedItem is PS5FileItem item && item.IsDirectory)
                _ = LoadPS5DirectoryAsync(item.FullPath);
        }

        private void ClearSearchButton_Click(object? sender, RoutedEventArgs e)
        {
            SearchTextBox.Text = "";
            _searchQuery = "";
            ApplySearchFilter();
        }

        private void GoButton_Click(object? sender, RoutedEventArgs e)
        {
            string path = CurrentPathTextBox.Text?.Trim() ?? "";
            if (!string.IsNullOrEmpty(path)) _ = LoadPS5DirectoryAsync(path);
        }

        // ============================================================
        // NAS/SMB SUPPORT (ps5upload-style)
        // Adds network paths (\\server\share\folder) to upload queue
        // ============================================================
        private async void AddNasPath_Click(object? sender, RoutedEventArgs e)
        {
            try
            {
                string? nasPath = await ShowInputDialogAsync(
                    "Add NAS/Network Path", 
                    "Enter UNC path (\\\\server\\share\\folder):",
                    "\\\\NAS\\games");
                
                if (string.IsNullOrWhiteSpace(nasPath)) return;
                
                nasPath = nasPath.Trim();
                
                // Validate UNC path format
                if (!nasPath.StartsWith("\\\\") && !nasPath.StartsWith("//"))
                {
                    await ShowMessageAsync(
                        "Invalid path format.\n\n" +
                        "Use UNC format: \\\\server\\share\\folder\n" +
                        "Example: \\\\NAS\\games\\PS4\\MyGame",
                        "Invalid Path");
                    return;
                }
                
                // Normalize path (convert // to \\ for Windows)
                if (nasPath.StartsWith("//"))
                {
                    nasPath = "\\\\" + nasPath.Substring(2).Replace("/", "\\");
                }
                
                Log($"🌐 Checking NAS path: {nasPath}");
                
                // Check if path exists
                if (Directory.Exists(nasPath))
                {
                    // It's a directory - add it
                    DirectoryInfo dirInfo = new DirectoryInfo(nasPath);
                    _localFiles.Add(new LocalFileItem { 
                        Name = dirInfo.Name, 
                        FullPath = nasPath, 
                        Icon = "🌐", 
                        IsDirectory = true, 
                        Size = 0 
                    });
                    Log($"✅ Added NAS folder: {dirInfo.Name} ({nasPath})");
                }
                else if (File.Exists(nasPath))
                {
                    // It's a file - add it
                    FileInfo fileInfo = new FileInfo(nasPath);
                    
                    // Check if it's a ZIP file
                    if (fileInfo.Extension.Equals(".zip", StringComparison.OrdinalIgnoreCase))
                    {
                        await ExtractAndAddZipContents(nasPath, fileInfo.Name);
                    }
                    else
                    {
                        _localFiles.Add(new LocalFileItem { 
                            Name = fileInfo.Name, 
                            FullPath = nasPath, 
                            Icon = "🌐", 
                            IsDirectory = false, 
                            Size = fileInfo.Length 
                        });
                        Log($"✅ Added NAS file: {fileInfo.Name} ({FormatFileSize(fileInfo.Length)})");
                    }
                }
                else
                {
                    Log($"❌ NAS path not found: {nasPath}");
                    await ShowMessageAsync(
                        $"Cannot access: {nasPath}\n\n" +
                        "Make sure:\n" +
                        "• The network share is accessible\n" +
                        "• You have permission to read it\n" +
                        "• The path is correct",
                        "NAS Path Not Found");
                }
            }
            catch (Exception ex)
            {
                Log($"❌ NAS error: {ex.Message}");
                await ShowMessageAsync($"Error accessing NAS path: {ex.Message}", "Error");
            }
        }

        private async void RenameMenuItem_Click(object? sender, RoutedEventArgs e)
        {
            if (PS5FilesListBox.SelectedItem is PS5FileItem item)
            {
                string? newName = await ShowInputDialogAsync("Rename", "New name:", item.Name);
                if (!string.IsNullOrWhiteSpace(newName))
                {
                    try
                    {
                        string newPath = Path.Combine(Path.GetDirectoryName(item.FullPath)?.Replace("\\", "/") ?? "", newName).Replace("\\", "/");
                        await _protocol.RenameAsync(item.FullPath, newPath);
                        Log($"✅ Renamed {item.Name} to {newName}");
                        await LoadPS5DirectoryAsync(_currentPS5Path);
                    }
                    catch (Exception ex) { await ShowMessageAsync($"Rename failed: {ex.Message}", "Error"); Log($"❌ Rename failed: {ex.Message}"); }
                }
            }
        }

        private async void CopyMenuItem_Click(object? sender, RoutedEventArgs e)
        {
            if (PS5FilesListBox.SelectedItem is PS5FileItem item)
            {
                string? destPath = await ShowInputDialogAsync("Copy To", "Destination path:", _currentPS5Path + "/" + item.Name);
                if (destPath != null)
                {
                    try { await _protocol.CopyFileAsync(item.FullPath, destPath); Log($"✅ Copied {item.Name} to {destPath}"); await LoadPS5DirectoryAsync(_currentPS5Path); }
                    catch (Exception ex) { await ShowMessageAsync($"Copy failed: {ex.Message}", "Error"); Log($"❌ Copy failed: {ex.Message}"); }
                }
            }
        }

        private async void MoveMenuItem_Click(object? sender, RoutedEventArgs e)
        {
            if (PS5FilesListBox.SelectedItem is PS5FileItem item)
            {
                string? destPath = await ShowInputDialogAsync("Move To", "Destination path:", _currentPS5Path + "/" + item.Name);
                if (destPath != null)
                {
                    try { await _protocol.RenameAsync(item.FullPath, destPath); Log($"✅ Moved {item.Name} to {destPath}"); await LoadPS5DirectoryAsync(_currentPS5Path); }
                    catch (Exception ex) { await ShowMessageAsync($"Move failed: {ex.Message}", "Error"); Log($"❌ Move failed: {ex.Message}"); }
                }
            }
        }

        private async void DeleteMenuItem_Click(object? sender, RoutedEventArgs e)
        {
            if (PS5FilesListBox.SelectedItem is PS5FileItem item)
            {
                if (await ShowConfirmAsync($"Delete {item.Name}?"))
                {
                    try
                    {
                        if (item.IsDirectory) { Log($"🗑️ Deleting folder: {item.Name}"); await _protocol.DeleteDirAsync(item.FullPath); Log($"✅ Folder deletion complete: {item.Name}"); }
                        else { Log($"🗑️ Deleting file: {item.Name}"); await _protocol.DeleteFileAsync(item.FullPath); Log($"✅ File deletion complete: {item.Name}"); }
                        await Task.Delay(1500);
                        await LoadPS5DirectoryAsync(_currentPS5Path);
                    }
                    catch (Exception ex) { await ShowMessageAsync($"Delete failed: {ex.Message}", "Error"); Log($"❌ Delete failed: {ex.Message}"); }
                }
            }
        }

        private async void DeleteSelectedMenuItem_Click(object? sender, RoutedEventArgs e)
        {
            var selectedItems = (PS5FilesListBox.SelectedItems ?? Array.Empty<object>()).Cast<PS5FileItem>().ToList();
            if (selectedItems.Count == 0) { await ShowMessageAsync("No items selected."); return; }

            int folderCount = selectedItems.Count(i => i.IsDirectory);
            int fileCount = selectedItems.Count(i => !i.IsDirectory);
            string message = $"Delete {selectedItems.Count} items?\n\n";
            if (folderCount > 0) message += $"📁 {folderCount} folders\n";
            if (fileCount > 0) message += $"📄 {fileCount} files";

            if (!await ShowConfirmAsync(message, "Confirm Bulk Delete")) return;

            Log($"🗑️ Starting bulk delete of {selectedItems.Count} items...");
            int successCount = 0, failCount = 0;
            foreach (var item in selectedItems)
            {
                try
                {
                    if (item.IsDirectory) await _protocol.DeleteDirAsync(item.FullPath);
                    else await _protocol.DeleteFileAsync(item.FullPath);
                    successCount++;
                    Log($"✅ Deleted: {item.Name}");
                    await Task.Delay(300);
                }
                catch (Exception ex) { Log($"❌ Failed to delete {item.Name}: {ex.Message}"); failCount++; }
            }
            Log($"🗑️ Bulk delete complete: {successCount} succeeded, {failCount} failed");
            await Task.Delay(500);
            await LoadPS5DirectoryAsync(_currentPS5Path);
            await ShowMessageAsync($"Bulk delete complete!\n\n✅ {successCount} deleted\n❌ {failCount} failed");
        }

        private async void DownloadMenuItem_Click(object? sender, RoutedEventArgs e)
        {
            if (PS5FilesListBox.SelectedItem is PS5FileItem item)
            {
                var topLevel = TopLevel.GetTopLevel(this);
                if (topLevel == null) return;

                if (item.IsDirectory)
                {
                    var folders = await topLevel.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = $"Select destination for '{item.Name}' folder" });
                    if (folders.Count == 0) return;
                    var folderPath = folders[0].TryGetLocalPath();
                    if (folderPath == null) return;
                    string localBasePath = Path.Combine(folderPath, item.Name);

                    try
                    {
                        Log($"⬇️ Downloading folder: {item.FullPath} → {localBasePath}");
                        _uploadCancellation = new CancellationTokenSource();
                        var ct = _uploadCancellation.Token;
                        int dlCallCount = 0;
                        var progress = new InlineProgress<DownloadFolderProgress>(p =>
                        {
                            if (++dlCallCount % 8 != 0 && p.Phase != "Complete") return;
                            Dispatcher.UIThread.Post(() =>
                            {
                                if (p.Phase == "Scanning")
                                    TotalProgressText.Text = $"Scanning: {p.FilesCompleted} files found... ({p.CurrentFile})";
                                else if (p.Phase == "Downloading")
                                {
                                    double percent = p.TotalBytes > 0 ? (double)p.BytesDownloaded / p.TotalBytes * 100 : 0;
                                    TotalProgressBar.Value = percent;
                                    TotalProgressText.Text = $"Downloading: {p.FilesCompleted}/{p.TotalFiles} files ({FormatFileSize(p.BytesDownloaded)}/{FormatFileSize(p.TotalBytes)}) [{percent:F1}%]";
                                    if (!string.IsNullOrEmpty(p.CurrentFile)) UploadFileNameText.Text = $"⬇️ {p.CurrentFile}";
                                }
                                else if (p.Phase == "Complete")
                                {
                                    TotalProgressBar.Value = 100;
                                    TotalProgressText.Text = $"Complete: {p.FilesCompleted}/{p.TotalFiles} files ({FormatFileSize(p.BytesDownloaded)})";
                                }
                            });
                        });
                        var (downloaded, failed, totalBytes) = await _protocol.DownloadFolderAsync(item.FullPath, localBasePath, _ps5IpAddress, progress, ct);
                        Log($"✅ Folder download complete: {downloaded} files ({FormatFileSize(totalBytes)}), {failed} failed");
                        await ShowMessageAsync($"Folder downloaded!\n\nFiles: {downloaded} downloaded, {failed} failed\nSize: {FormatFileSize(totalBytes)}\nSaved to: {localBasePath}", "Download Complete");
                    }
                    catch (Exception ex) { Log($"❌ Folder download error: {ex.Message}"); await ShowMessageAsync($"Folder download error: {ex.Message}", "Error"); }
                    finally { _uploadCancellation = null; TotalProgressBar.Value = 0; TotalProgressText.Text = ""; UploadFileNameText.Text = ""; }
                    return;
                }

                // Single file download
                var saveFile = await topLevel.StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions { Title = "Save Downloaded File", SuggestedFileName = item.Name });
                if (saveFile != null)
                {
                    var savePath = saveFile.TryGetLocalPath();
                    if (savePath == null) return;
                    try
                    {
                        Log($"⬇️ Downloading: {item.Name} ({FormatFileSize(item.Size)})");
                        int sfCallCount = 0;
                        var progress = new InlineProgress<UploadProgress>(p =>
                        {
                            if (++sfCallCount % 8 != 0 && p.BytesSent != p.TotalBytes) return;
                            Dispatcher.UIThread.Post(() =>
                            {
                                double percent = p.TotalBytes > 0 ? (double)p.BytesSent / p.TotalBytes * 100 : 0;
                                TotalProgressBar.Value = percent;
                                TotalProgressText.Text = $"Downloading: {FormatFileSize(p.BytesSent)}/{FormatFileSize(p.TotalBytes)} ({percent:F1}%) @ {FormatFileSize((long)p.SpeedBytesPerSecond)}/s";
                            });
                        });
                        bool success = await _protocol.DownloadFileAsync(item.FullPath, savePath, progress);
                        if (success) { Log($"✅ Downloaded: {item.Name} → {savePath}"); await ShowMessageAsync($"File downloaded successfully!\n\nSaved to: {savePath}", "Success"); }
                        else { Log($"❌ Download failed: {item.Name}"); await ShowMessageAsync("Download failed", "Error"); }
                    }
                    catch (Exception ex) { Log($"❌ Download error: {ex.Message}"); await ShowMessageAsync($"Download error: {ex.Message}", "Error"); }
                    finally { TotalProgressBar.Value = 0; TotalProgressText.Text = ""; }
                }
            }
        }

        // ============================================================
        // LOG BUTTONS
        // ============================================================
        private void ClearLogButton_Click(object? sender, RoutedEventArgs e) { LogTextBox.Text = ""; Log("Log cleared"); }

        private async void CopyLogButton_Click(object? sender, RoutedEventArgs e)
        {
            try
            {
                var clipboard = TopLevel.GetTopLevel(this)?.Clipboard;
                if (clipboard != null)
                {
                    var item = new Avalonia.Input.DataTransferItem();
                    item.Set(Avalonia.Input.DataFormat.Text, LogTextBox.Text ?? "");
                    var data = new Avalonia.Input.DataTransfer();
                    data.Add(item);
                    await clipboard.SetDataAsync(data);
                    Log("📋 Log copied to clipboard!");
                }
            }
            catch (Exception ex) { Log($"⚠️ Clipboard error: {ex.Message}"); }
        }

        private async Task<PS5StorageInfo?> FetchStorageInfoWithFallbackAsync()
        {
            bool uploadsIdle = (_uploadCancellation == null || _uploadCancellation.IsCancellationRequested) && _activeTaskCount == 0;
            if (_protocol.IsConnected && uploadsIdle)
                return await _protocol.ListStorageAsync();

            var tempProtocol = new PS5Protocol();
            try
            {
                if (!await tempProtocol.ConnectAsync(_ps5IpAddress)) { Log("❌ Storage refresh: failed to open temporary connection"); return null; }
                return await tempProtocol.ListStorageAsync();
            }
            finally { tempProtocol.Dispose(); }
        }
    }
}
