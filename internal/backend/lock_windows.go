//go:build windows

package backend

import (
	"os"

	"golang.org/x/sys/windows"
)

// 操作系统持有的锁在进程崩溃时自动释放，残留 .lock 文件不会阻止重启。
func lockFile(file *os.File) error {
	return windows.LockFileEx(windows.Handle(file.Fd()), windows.LOCKFILE_EXCLUSIVE_LOCK|windows.LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &windows.Overlapped{})
}
