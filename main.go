package main

import (
	"flag"
	"fmt"
	"os"
)

func main() {
	loadCfg()
	cli := flag.String("cli", "", "status | install | restore (без окна)")
	malik := flag.Bool("malik", false, "с моделью Малика (только с --cli install)")
	game := flag.String("game", "", "папка игры")
	flag.Parse()
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
	if g == "" {
		fmt.Println("game folder not found, use --game")
		os.Exit(2)
	}
	log := func(ru, en string) { fmt.Println(ru + " | " + en) }
	var err error
	switch *cli {
	case "status":
		fmt.Println(detectState(g))
	case "install", "restore":
		if gameRunning() {
			fmt.Println("game is running")
			os.Exit(1)
		}
		if err = checkWritable(g); err == nil {
			if *cli == "install" {
				err = doInstall(g, *malik, log)
			} else {
				err = doRestore(g, log)
			}
		}
	default:
		fmt.Println("unknown --cli value")
		os.Exit(2)
	}
	if err != nil {
		fmt.Println("ERROR:", err)
		os.Exit(1)
	}
}
