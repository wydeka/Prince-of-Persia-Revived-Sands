package main

import (
	"bytes"
	_ "embed"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// Умный ИИ врагов: прокси-библиотека dinput8.dll и её настройки revived.ini.
// Файлы лежат в папке payload и вшиваются в .exe при сборке.

//go:embed payload/dinput8.dll
var dllBytes []byte

//go:embed payload/revived.ini
var iniBytes []byte

const (
	dllName = "dinput8.dll"
	dllBak  = "dinput8.dll.before_revived" // чужой dinput8.dll, который стоял до нас
	iniName = "revived.ini"
	iniBak  = "revived.ini.bak"
)

// файлы, которые создаёт сама библиотека во время игры
var modLogs = []string{"revived_log.txt", "revived_trace.txt", "revived_entities.txt", "revived_fights.csv"}

// Opts - что именно ставить.
type Opts struct {
	Data     bool // правки таймингов в архивах .forge
	AI       bool // умный ИИ (dinput8.dll + revived.ini)
	Malik    bool // модель Малика вместо Принца (только вместе с Data)
	ResetIni bool // перезаписать revived.ini настройками по умолчанию
}

func exists(p string) bool { _, err := os.Stat(p); return err == nil }

// aiState: none | current | older | foreign
//
//	none    - dinput8.dll нет
//	current - ровно та библиотека, что вшита в установщик
//	older   - наша, но другой версии (внутри есть имя нашего лога)
//	foreign - чужой dinput8.dll (например, другой мод)
func aiState(game string) string {
	b, err := os.ReadFile(filepath.Join(game, dllName))
	if err != nil {
		return "none"
	}
	if bytes.Equal(b, dllBytes) {
		return "current"
	}
	if bytes.Contains(b, []byte("revived_trace.txt")) {
		return "older"
	}
	return "foreign"
}

// writeAtomic пишет во временный файл и подменяет им целевой: при сбое старый файл остаётся целым.
func writeAtomic(path string, data []byte) error {
	tmp := path + ".rs_tmp"
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	if _, err = f.Write(data); err == nil {
		err = f.Sync()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	if err = os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func installAI(game string, resetIni bool, log Logf) error {
	dll := filepath.Join(game, dllName)
	switch aiState(game) {
	case "foreign":
		bak := filepath.Join(game, dllBak)
		if exists(bak) {
			bak += "." + time.Now().Format("20060102-150405")
		}
		if err := os.Rename(dll, bak); err != nil {
			return merr("Не удалось убрать чужой dinput8.dll (%v). Закройте игру и повторите.", "Cannot move the foreign dinput8.dll away (%v). Close the game and try again.", err)
		}
		log("В папке был чужой dinput8.dll - сохранён как "+filepath.Base(bak)+".", "A foreign dinput8.dll was in the folder - saved as "+filepath.Base(bak)+".")
		fallthrough
	case "none", "older":
		if err := writeAtomic(dll, dllBytes); err != nil {
			return merr("Не удалось записать dinput8.dll (%v). Закройте игру и запустите установщик от имени администратора.", "Cannot write dinput8.dll (%v). Close the game and run the installer as administrator.", err)
		}
		log("Умный ИИ врагов (dinput8.dll) установлен.", "Smart enemy AI (dinput8.dll) installed.")
	case "current":
		log("Умный ИИ врагов уже установлен (актуальная версия).", "Smart enemy AI is already installed (current version).")
	}
	ini := filepath.Join(game, iniName)
	switch {
	case !exists(ini):
		if err := writeAtomic(ini, iniBytes); err != nil {
			return merr("Не удалось записать revived.ini (%v).", "Cannot write revived.ini (%v).", err)
		}
		log("Настройки revived.ini созданы.", "revived.ini settings created.")
	case resetIni:
		if old, err := os.ReadFile(ini); err == nil && !bytes.Equal(old, iniBytes) {
			writeAtomic(filepath.Join(game, iniBak), old)
			log("Ваши прежние настройки сохранены как "+iniBak+".", "Your previous settings were saved as "+iniBak+".")
		}
		if err := writeAtomic(ini, iniBytes); err != nil {
			return merr("Не удалось записать revived.ini (%v).", "Cannot write revived.ini (%v).", err)
		}
		log("Настройки сброшены к стандартным.", "Settings reset to defaults.")
	default:
		log("Ваш revived.ini оставлен как есть.", "Your revived.ini was kept as it is.")
	}
	if aiState(game) != "current" {
		return merr("dinput8.dll записан, но проверка не прошла.", "dinput8.dll was written but verification failed.")
	}
	return nil
}

func restoreAI(game string, log Logf) error {
	dll := filepath.Join(game, dllName)
	bak := filepath.Join(game, dllBak)
	switch aiState(game) {
	case "current", "older":
		if err := os.Remove(dll); err != nil {
			return merr("Не удалось удалить dinput8.dll (%v). Закройте игру и запустите установщик от имени администратора.", "Cannot delete dinput8.dll (%v). Close the game and run the installer as administrator.", err)
		}
		log("Умный ИИ врагов удалён.", "Smart enemy AI removed.")
	case "foreign":
		log("В папке чужой dinput8.dll - не трогаю.", "A foreign dinput8.dll is in the folder - left untouched.")
	}
	if aiState(game) == "none" && exists(bak) {
		if err := os.Rename(bak, dll); err == nil {
			log("Прежний dinput8.dll возвращён на место.", "The previous dinput8.dll was put back.")
		}
	}
	ini := filepath.Join(game, iniName)
	if b, err := os.ReadFile(ini); err == nil {
		if bytes.Equal(b, iniBytes) {
			os.Remove(ini)
		} else {
			log("revived.ini изменён вами - оставлен (можно удалить вручную).", "revived.ini was changed by you - kept (delete it by hand if you like).")
		}
	}
	for _, n := range modLogs {
		os.Remove(filepath.Join(game, n))
	}
	return nil
}

// dirWritable: можно ли создавать файлы в папке игры (Program Files без прав администратора - нельзя).
func dirWritable(game string) bool {
	f, err := os.CreateTemp(game, ".rs_write_test*")
	if err != nil {
		return false
	}
	name := f.Name()
	f.Close()
	os.Remove(name)
	return true
}

func installAll(game string, o Opts, log Logf) error {
	if !o.Data && !o.AI {
		return merr("Ничего не выбрано для установки.", "Nothing is selected to install.")
	}
	if o.Data {
		if err := doInstall(game, o.Malik, log); err != nil {
			return err
		}
	}
	if o.AI {
		if err := installAI(game, o.ResetIni, log); err != nil {
			return err
		}
	}
	return nil
}

func restoreAll(game string, log Logf) error {
	if detectState(game) == "bad_folder" {
		return merr("Это не папка игры.", "This is not the game folder.")
	}
	if err := restoreAI(game, log); err != nil {
		return err
	}
	return doRestore(game, log)
}

func describe(game string) string {
	return fmt.Sprintf("data=%s ai=%s", detectState(game), aiState(game))
}
