using Foundation;
using UIKit;
using Avalonia;
using Avalonia.Controls;
using Avalonia.iOS;
using Avalonia.Media;

namespace PS5SuiteAndroid.iOS;

// The UIApplicationDelegate for the application. This class is responsible for launching the
// User Interface of the application, as well as listening (and optionally responding) to
// application events from iOS.
[Register("AppDelegate")]
#pragma warning disable CA1711 // Identifiers should not have incorrect suffix
public partial class AppDelegate : AvaloniaAppDelegate<App>
#pragma warning restore CA1711 // Identifiers should not have incorrect suffix
{
    public override bool FinishedLaunching(UIApplication application, NSDictionary launchOptions)
    {
        // Same wiring as the Android head: sandboxed, user-browsable writable
        // dirs (no permissions needed) + in-app browser for external links.
        var docs = System.Environment.GetFolderPath(System.Environment.SpecialFolder.MyDocuments);
        PS5SuiteAndroid.AppPaths.DownloadsDirProvider = () => docs;
        PS5SuiteAndroid.AppPaths.CacheDirProvider = () => System.IO.Path.Combine(docs, "..", "Library", "Caches");
        PS5SuiteAndroid.AppPaths.UrlOpener = url =>
        {
            try
            {
                if (NSUrl.TryParse(url, out var nsUrl))
                    UIApplication.SharedApplication.OpenUrl(nsUrl);
            }
            catch { }
        };
        return base.FinishedLaunching(application, launchOptions);
    }

    protected override AppBuilder CustomizeAppBuilder(AppBuilder builder)
    {
        return base.CustomizeAppBuilder(builder)
            .WithInterFont();
    }
}
