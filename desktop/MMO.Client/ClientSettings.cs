using System.IO;
using System.Text.Json;

namespace Mmo.Client.Desktop;

public sealed class ClientSettings
{
    public string Address { get; set; } = "127.0.0.1:7000";
    public string ProfileName { get; set; } = "desktop-check";
    public bool UseTls { get; set; }

    public static ClientSettings Load(string directory)
    {
        // 运行配置只在本机保存；交付目录可提供不含秘密的初始地址。
        foreach (string file in new[] { Path.Combine(directory, "settings.json"), Path.Combine(AppContext.BaseDirectory, "client.settings.json") })
        {
            if (!File.Exists(file)) continue;
            try { return JsonSerializer.Deserialize<ClientSettings>(File.ReadAllText(file), new JsonSerializerOptions { PropertyNameCaseInsensitive = true }) ?? new(); }
            catch (JsonException) { }
            catch (IOException) { }
        }
        return new();
    }

    public void Save(string directory)
    {
        Directory.CreateDirectory(directory);
        string file = Path.Combine(directory, "settings.json");
        string temp = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
            File.Move(temp, file, true);
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
}
