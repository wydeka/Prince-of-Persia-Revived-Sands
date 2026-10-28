//go:build windows

package main

import (
	"os"
	"os/exec"
	"regexp"
	"strings"
	"syscall"
	"unsafe"
)

func hidden(c *exec.Cmd) *exec.Cmd {
	c.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000}
	return c
}

func regQuery(key, val string) string {
	out, err := hidden(exec.Command("reg", "query", key, "/v", val)).Output()
	if err != nil {
		return ""
	}
	re := regexp.MustCompile(`(?m)^\s*` + regexp.QuoteMeta(val) + `\s+REG_SZ\s+(.+?)\s*$`)
	if m := re.FindStringSubmatch(string(out)); m != nil {
		return m[1]
	}
	return ""
}

func gameRunning() bool {
	out, err := hidden(exec.Command("tasklist", "/FI", "IMAGENAME eq "+gameExe)).Output()
	return err == nil && strings.Contains(strings.ToLower(string(out)), strings.ToLower(gameExe))
}

func browseFolder() string {
	ps := `Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.FolderBrowserDialog; ` +
		`$d.Description = 'Prince of Persia Forgotten Sands'; if ($d.ShowDialog() -eq 'OK') { [Console]::OutputEncoding=[Text.Encoding]::UTF8; Write-Output $d.SelectedPath }`
	out, err := hidden(exec.Command("powershell", "-NoProfile", "-STA", "-Command", ps)).Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

func shellExecute(verb, file, args string) bool {
	sh := syscall.NewLazyDLL("shell32.dll").NewProc("ShellExecuteW")
	v, _ := syscall.UTF16PtrFromString(verb)
	f, _ := syscall.UTF16PtrFromString(file)
	a, _ := syscall.UTF16PtrFromString(args)
	r, _, _ := sh.Call(0, uintptr(unsafe.Pointer(v)), uintptr(unsafe.Pointer(f)), uintptr(unsafe.Pointer(a)), 0, 1)
	return r > 32
}

func openBrowser(url string) bool { return shellExecute("open", url, "") }

func relaunchElevated(game string) bool {
	exe, err := os.Executable()
	if err != nil {
		return false
	}
	args := ""
	if game != "" {
		args = `--game "` + game + `"`
	}
	return shellExecute("runas", exe, args)
}

func msgBox(title, text string) {
	mb := syscall.NewLazyDLL("user32.dll").NewProc("MessageBoxW")
	t, _ := syscall.UTF16PtrFromString(title)
	x, _ := syscall.UTF16PtrFromString(text)
	mb.Call(0, uintptr(unsafe.Pointer(x)), uintptr(unsafe.Pointer(t)), 0x40)
}
