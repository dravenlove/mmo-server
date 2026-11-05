using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace Mmo.Client.Core;

internal static class WindowsDpapi
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Blob
    {
        public int Size;
        public IntPtr Data;
    }

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CryptProtectData(ref Blob input, string? description,
        ref Blob entropy, IntPtr reserved, IntPtr prompt, uint flags, out Blob output);

    [DllImport("crypt32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CryptUnprotectData(ref Blob input, IntPtr description,
        ref Blob entropy, IntPtr reserved, IntPtr prompt, uint flags, out Blob output);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LocalFree(IntPtr memory);

    public static byte[] Protect(byte[] plaintext, byte[] entropy) => Transform(plaintext, entropy, protect: true);
    public static byte[] Unprotect(byte[] ciphertext, byte[] entropy) => Transform(ciphertext, entropy, protect: false);

    private static byte[] Transform(byte[] data, byte[] entropyBytes, bool protect)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("本地凭据使用 Windows 用户加密保护。");
        var input = new Blob { Size = data.Length, Data = Marshal.AllocHGlobal(data.Length) };
        var entropy = new Blob { Size = entropyBytes.Length, Data = Marshal.AllocHGlobal(entropyBytes.Length) };
        var output = new Blob();
        try
        {
            Marshal.Copy(data, 0, input.Data, input.Size);
            Marshal.Copy(entropyBytes, 0, entropy.Data, entropy.Size);
            // CurrentUser 模式且禁止弹出系统对话框；不使用 LocalMachine 共享密钥。
            const uint cryptProtectUiForbidden = 1;
            var ok = protect
                ? CryptProtectData(ref input, null, ref entropy, IntPtr.Zero, IntPtr.Zero,
                    cryptProtectUiForbidden, out output)
                : CryptUnprotectData(ref input, IntPtr.Zero, ref entropy, IntPtr.Zero,
                    IntPtr.Zero, cryptProtectUiForbidden, out output);
            if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error());
            var result = new byte[output.Size];
            Marshal.Copy(output.Data, result, 0, result.Length);
            return result;
        }
        finally
        {
            Clear(input);
            Marshal.FreeHGlobal(input.Data);
            Clear(entropy);
            Marshal.FreeHGlobal(entropy.Data);
            if (output.Data != IntPtr.Zero)
            {
                if (!protect) Clear(output);
                _ = LocalFree(output.Data);
            }
        }
    }

    private static void Clear(Blob blob)
    {
        if (blob.Data == IntPtr.Zero || blob.Size <= 0) return;
        var zeros = new byte[blob.Size];
        Marshal.Copy(zeros, 0, blob.Data, blob.Size);
        CryptographicOperations.ZeroMemory(zeros);
    }
}
