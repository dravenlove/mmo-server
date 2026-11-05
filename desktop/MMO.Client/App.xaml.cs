using System.Windows;

namespace Mmo.Client.Desktop;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += (_, args) =>
        {
            MessageBox.Show("客户端发生错误：" + args.Exception.Message, "MMO 联调客户端", MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };
        var window = new MainWindow(e.Args);
        MainWindow = window;
        window.Show();
    }
}
