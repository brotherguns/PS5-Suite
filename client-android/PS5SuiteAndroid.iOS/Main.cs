using Foundation;
using UIKit;

namespace PS5SuiteAndroid.iOS;

public class Application
{
    // This is the main entry point of the application.
    static void Main(string[] args)
    {
        // Same wiring as the Android head: sandboxed, user-browsable writable
        // dirs (no permissions needed) + in-app browser for external links.
        // (Done here instead of AppDelegate because AvaloniaAppDelegate does
        // not expose an overridable launch hook.)
        var docs = System.Environment.GetFolderPath(System.Environment.SpecialFolder.MyDocuments);
        PS5SuiteAndroid.AppPaths.DownloadsDirProvider = () => docs;
        PS5SuiteAndroid.AppPaths.CacheDirProvider = () => System.IO.Path.Combine(docs, "..", "Library", "Caches");
        PS5SuiteAndroid.AppPaths.UrlOpener = url =>
        {
            try
            {
                var nsUrl = NSUrl.FromString(url);
                if (nsUrl is not null)
                    UIApplication.SharedApplication.OpenUrl(nsUrl, new NSDictionary(), null);
            }
            catch { }
        };

        // if you want to use a different Application Delegate class from "AppDelegate"
        // you can specify it here.
        UIApplication.Main(args, null, typeof(AppDelegate));
    }
}
