package main

import (
	"flag"
	"fmt"
	"os"
)

// out пишет строку в консоль и, если задан --logfile, в файл (UTF-8 с BOM, его читает мастер установки).
var logPath string

func out(s string) {
	fmt.Println(s)
	if logPath == "" {
		return
	}
	f, err := os.OpenFile(logPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return
	}
	defer f.Close()
	if st, e := f.Stat(); e == nil && st.Size() == 0 {
		f.Write([]byte{0xEF, 0xBB, 0xBF})
	}
	f.WriteString(s + "\r\n")
}

func fail(code int, s string) {
	out(s)
	os.Exit(code)
}

func main() {
	loadCfg()
	cli := flag.String("cli", "", "status | install | restore | find (без окна)")
	malik := flag.Bool("malik", false, "с моделью Малика (только с --cli install)")
	noAI := flag.Bool("no-ai", false, "не ставить умный ИИ (dinput8.dll)")
	noData := flag.Bool("no-data", false, "не трогать архивы .forge (тайминги)")
	resetIni := flag.Bool("reset-ini", false, "сбросить revived.ini к стандартному")
	game := flag.String("game", "", "папка игры")
	logfile := flag.String("logfile", "", "дублировать вывод в файл")
	flag.Parse()
	logPath = *logfile
	if *cli == "" {
		runGUI(*game)
		return
	}
	g := *game
	if g == "" {
		g = os.Getenv("POP_DIR")
	}
	if g == "" {
		g = findGame()
	}
	if *cli == "find" {
		if g == "" {
			os.Exit(2)
		}
		out(g)
		return
	}
	if g == "" {
		fail(2, "Папка игры не найдена, укажите --game | Game folder not found, use --game")
	}
	log := func(ru, en string) { out(ru + " | " + en) }
	var err error
	switch *cli {
	case "status":
		out(describe(g))
	case "install", "restore":
		if gameRunning() {
			fail(1, "ERROR: Игра запущена. Закройте её и повторите. / The game is running. Close it and try again.")
		}
		if detectState(g) == "bad_folder" {
			fail(1, "ERROR: Это не папка игры. / This is not the game folder.")
		}
		o := Opts{Data: !*noData, AI: !*noAI, Malik: *malik, ResetIni: *resetIni}
		if err = checkWritable(g, *cli == "restore" || o.Data); err == nil {
			if *cli == "install" {
				err = installAll(g, o, log)
			} else {
				err = restoreAll(g, log)
			}
		}
	default:
		fail(2, "unknown --cli value")
	}
	if err != nil {
		fail(1, "ERROR: "+err.Error())
	}
}
