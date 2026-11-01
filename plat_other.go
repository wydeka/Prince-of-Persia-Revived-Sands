//go:build !windows

package main

import (
	"os/exec"
	"strings"
)

func regQuery(key, val string) string   { return "" }
func gameRunning() bool                 { return false }
func browseFolder() string              { return "" }
func openPath(p string) bool            { return false }
func relaunchElevated(game string) bool { return false }
func msgBox(title, text string)         { println(title + ": " + text) }
func openBrowser(url string) bool {
	println(url)
	return exec.Command("xdg-open", strings.TrimSpace(url)).Start() == nil
}
