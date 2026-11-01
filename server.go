package main

import (
	"crypto/rand"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

//go:embed ui.html
var uiHTML string

const (
	ModTitle   = "Prince of Persia: The Revived Sands"
	ModVersion = "1.1.3.11 (Beta)"
	Author     = "wydeka"
)

// DiscordURL можно переопределить при сборке (-X main.DiscordURL=...) или файлом discord.txt рядом с .exe
var DiscordURL = "https://discord.gg/HRFtZ2sNJs"

const GithubURL = "https://github.com/wydeka/Prince-of-Persia-Revived-Sands"

type Line struct {
	RU string `json:"ru"`
	EN string `json:"en"`
}

type Srv struct {
	mu       sync.Mutex
	token    string
	busy     bool
	log      []Line
	err      *Line
	access   bool
	state    string
	ai       string
	lastPing time.Time
	pinged   bool
	quit     chan struct{}
	argGame  string
}

func discordURL() string {
	if exe, err := os.Executable(); err == nil {
		if b, err := os.ReadFile(filepath.Join(filepath.Dir(exe), "discord.txt")); err == nil {
			if s := strings.TrimSpace(string(b)); strings.HasPrefix(s, "http") {
				return s
			}
		}
	}
	return DiscordURL
}

func findGame() string {
	var libs []string
	for _, k := range []string{`HKLM\SOFTWARE\WOW6432Node\Valve\Steam`, `HKLM\SOFTWARE\Valve\Steam`, `HKCU\Software\Valve\Steam`} {
		for _, v := range []string{"InstallPath", "SteamPath"} {
			if out := regQuery(k, v); out != "" {
				libs = append(libs, out)
			}
		}
	}
	libs = append(libs, `C:\Program Files (x86)\Steam`, `C:\Program Files\Steam`)
	for _, d := range "CDEFGH" {
		libs = append(libs, fmt.Sprintf(`%c:\SteamLibrary`, d), fmt.Sprintf(`%c:\Steam`, d), fmt.Sprintf(`%c:\Program Files (x86)\Steam`, d))
	}
	re := regexp.MustCompile(`"path"\s+"([^"]+)"`)
	all := append([]string{}, libs...)
	for _, r := range libs {
		if b, err := os.ReadFile(filepath.Join(r, "steamapps", "libraryfolders.vdf")); err == nil {
			for _, m := range re.FindAllStringSubmatch(string(b), -1) {
				all = append(all, strings.ReplaceAll(m[1], `\\`, `\`))
			}
		}
	}
	imRe := regexp.MustCompile(`"installdir"\s+"([^"]+)"`)
	seen := map[string]bool{}
	for _, lib := range all {
		lib = filepath.Clean(lib)
		if seen[strings.ToLower(lib)] {
			continue
		}
		seen[strings.ToLower(lib)] = true
		names := []string{"Prince of Persia Forgotten Sands"}
		if b, err := os.ReadFile(filepath.Join(lib, "steamapps", "appmanifest_33320.acf")); err == nil {
			if m := imRe.FindStringSubmatch(string(b)); m != nil {
				names = append([]string{m[1]}, names...)
			}
		}
		for _, n := range names {
			p := filepath.Join(lib, "steamapps", "common", n)
			okAll := true
			for _, f := range requiredFiles {
				if st, err := os.Stat(filepath.Join(p, f)); err != nil || st.IsDir() {
					okAll = false
					break
				}
			}
			if okAll {
				return p
			}
		}
	}
	return ""
}

func (s *Srv) addLog(ru, en string) {
	s.mu.Lock()
	s.log = append(s.log, Line{ru, en})
	s.mu.Unlock()
}

func (s *Srv) runTask(kind, game string, o Opts) {
	var err error
	func() {
		defer func() {
			if r := recover(); r != nil {
				err = fmt.Errorf("%v", r)
			}
		}()
		if kind == "check" {
			if detectState(game) != "bad_folder" && !dirWritable(game) {
				s.mu.Lock()
				s.access = true // подсказать про права администратора заранее
				s.mu.Unlock()
			}
			return
		}
		if gameRunning() {
			err = merr("Игра запущена. Закройте её и повторите.", "The game is running. Close it and try again.")
			return
		}
		if detectState(game) == "bad_folder" {
			err = merr("Это не папка игры.", "This is not the game folder.")
			return
		}
		if err = checkWritable(game, kind == "restore" || o.Data); err != nil {
			return
		}
		if kind == "install" {
			err = installAll(game, o, s.addLog)
		} else {
			err = restoreAll(game, s.addLog)
		}
	}()
	st, ai := detectState(game), aiState(game)
	s.mu.Lock()
	s.state, s.ai = st, ai
	if err != nil {
		l := Line{err.Error(), err.Error()}
		if me, ok := err.(*ModErr); ok {
			l = Line{me.RU, me.EN}
		} else if ae, ok := err.(*AccessErr); ok {
			l = Line{ae.RU, ae.EN}
			s.access = true
		}
		s.err = &l
	}
	s.busy = false
	s.mu.Unlock()
}

func (s *Srv) auth(w http.ResponseWriter, r *http.Request) bool {
	if r.URL.Query().Get("t") != s.token {
		http.Error(w, "forbidden", 403)
		return false
	}
	return true
}

func jsonOut(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	json.NewEncoder(w).Encode(v)
}

func (s *Srv) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" || r.URL.Query().Get("t") != s.token {
			http.Error(w, "forbidden", 403)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		fmt.Fprint(w, strings.ReplaceAll(uiHTML, "__TOKEN__", s.token))
	})
	mux.HandleFunc("/api/info", func(w http.ResponseWriter, r *http.Request) {
		if !s.auth(w, r) {
			return
		}
		game := s.argGame
		if game == "" {
			game = findGame()
		}
		jsonOut(w, map[string]any{"game": game, "title": ModTitle, "version": ModVersion, "author": Author, "discord": discordURL(), "github": GithubURL})
	})
	mux.HandleFunc("/api/run", func(w http.ResponseWriter, r *http.Request) {
		if !s.auth(w, r) {
			return
		}
		q := r.URL.Query()
		kind := q.Get("kind")
		game := strings.Trim(strings.TrimSpace(q.Get("game")), `"`)
		if kind != "check" && kind != "install" && kind != "restore" {
			http.Error(w, "bad kind", 400)
			return
		}
		s.mu.Lock()
		if s.busy {
			s.mu.Unlock()
			jsonOut(w, map[string]any{"ok": false})
			return
		}
		s.busy, s.err, s.access, s.state, s.ai = true, nil, false, "", ""
		if kind != "check" {
			s.log = nil
		}
		s.mu.Unlock()
		o := Opts{Data: q.Get("data") == "1", AI: q.Get("ai") == "1", Malik: q.Get("malik") == "1", ResetIni: q.Get("reset") == "1"}
		go s.runTask(kind, game, o)
		jsonOut(w, map[string]any{"ok": true})
	})
	mux.HandleFunc("/api/poll", func(w http.ResponseWriter, r *http.Request) {
		if !s.auth(w, r) {
			return
		}
		s.mu.Lock()
		defer s.mu.Unlock()
		jsonOut(w, map[string]any{"busy": s.busy, "log": s.log, "error": s.err, "access": s.access, "state": s.state, "ai": s.ai})
	})
	mux.HandleFunc("/api/ping", func(w http.ResponseWriter, r *http.Request) {
		if !s.auth(w, r) {
			return
		}
		s.mu.Lock()
		s.lastPing, s.pinged = time.Now(), true
		s.mu.Unlock()
		jsonOut(w, map[string]any{"ok": true})
	})
	mux.HandleFunc("/api/browse", func(w http.ResponseWriter, r *http.Request) {
		if !s.auth(w, r) {
			return
		}
		jsonOut(w, map[string]any{"path": browseFolder()})
	})
	mux.HandleFunc("/api/open", func(w http.ResponseWriter, r *http.Request) {
		if !s.auth(w, r) {
			return
		}
		game := strings.Trim(strings.TrimSpace(r.URL.Query().Get("game")), `"`)
		ok := detectState(game) != "bad_folder" && openPath(game)
		jsonOut(w, map[string]any{"ok": ok})
	})
	mux.HandleFunc("/api/elevate", func(w http.ResponseWriter, r *http.Request) {
		if !s.auth(w, r) {
			return
		}
		ok := relaunchElevated(strings.Trim(r.URL.Query().Get("game"), `"`))
		jsonOut(w, map[string]any{"ok": ok})
		if ok {
			go func() { time.Sleep(500 * time.Millisecond); close(s.quit) }()
		}
	})
	mux.HandleFunc("/api/quit", func(w http.ResponseWriter, r *http.Request) {
		if !s.auth(w, r) {
			return
		}
		jsonOut(w, map[string]any{"ok": true})
		go func() { time.Sleep(300 * time.Millisecond); close(s.quit) }()
	})
	return mux
}

func runGUI(argGame string) {
	b := make([]byte, 16)
	rand.Read(b)
	s := &Srv{token: hex.EncodeToString(b), quit: make(chan struct{}), argGame: argGame}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		msgBox(ModTitle, "Cannot start local server: "+err.Error())
		return
	}
	url := fmt.Sprintf("http://%s/?t=%s", ln.Addr().String(), s.token)
	go http.Serve(ln, s.handler())
	if !openBrowser(url) {
		msgBox(ModTitle, "Open this address in your browser / Откройте этот адрес в браузере:\n\n"+url)
	}
	start := time.Now()
	tick := time.NewTicker(2 * time.Second)
	for {
		select {
		case <-s.quit:
			return
		case <-tick.C:
			s.mu.Lock()
			idle := time.Since(s.lastPing) > 25*time.Second
			gone := (s.pinged && idle) || (!s.pinged && time.Since(start) > 3*time.Minute)
			busy := s.busy
			s.mu.Unlock()
			if gone && !busy {
				return
			}
		}
	}
}
