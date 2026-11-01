package main

import (
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// Тест на «синтетической» папке игры: архивы создаются из самого cfg.json
// (оригинальные куски + размеры + заголовки), поэтому настоящая игра не нужна.
// Настоящую папку можно проверить так: POP_TEST_DIR=<копия папки игры> go test -run TestRealCopy -v .

var archSizes = map[string]int64{
	"DataPC_01-03.forge":  674430976,
	"DataPC_actors.forge": 260177920,
	"DataPC.forge":        62455808,
}

func put32(f *os.File, off int64, v int) {
	if _, err := f.WriteAt(u32(v), off); err != nil {
		panic(err)
	}
}

func makeGame(t *testing.T) string {
	dir := t.TempDir()
	files := map[string]*os.File{}
	for n, sz := range archSizes {
		f, err := os.Create(filepath.Join(dir, n))
		if err != nil {
			t.Fatal(err)
		}
		if err = f.Truncate(sz); err != nil {
			t.Skip("не удалось создать разреженный файл:", err)
		}
		files[n] = f
		defer f.Close()
	}
	// заголовок actors: какая запись сейчас «Принц»
	hb := make([]byte, 8)
	binary.LittleEndian.PutUint64(hb, uint64(princeOff))
	files[headA].WriteAt(hb, 1166)
	mb := make([]byte, 8)
	binary.LittleEndian.PutUint64(mb, uint64(malikOff))
	files[headA].WriteAt(mb, 1230)
	for i := range CFG {
		j := &CFG[i]
		f := files[j.Archive]
		put32(f, j.Entry+headerSizePos, j.Size0)
		if j.IndexPos != nil {
			put32(f, int64(*j.IndexPos), j.Size0)
			put32(f, int64(*j.TablePos), j.Size0)
		}
		for k, s := range j.RestoreSegs {
			// «оригинальный» кусок лежит в restore-данных, а его место - в координатах оригинала (Start у apply_segs)
			if _, err := f.WriteAt(s.Data, j.Entry+int64(j.ApplySegs[k].Start)); err != nil {
				t.Fatal(err)
			}
		}
		// 2 байта в заголовках кусков, которые установщик правит (лежат между кусками данных)
		for _, sl := range j.SlotsApply {
			shift, inside := 0, false
			for _, sg := range j.ApplySegs {
				ns := sg.Start + shift
				ne := ns + len(sg.Data)
				if sl.Pos >= ns && sl.Pos < ne {
					inside = true
					break
				}
				if ne <= sl.Pos {
					shift += len(sg.Data) - sg.Length
				}
			}
			if !inside {
				f.WriteAt(sl.Old, j.Entry+int64(sl.Pos-shift))
			}
		}
	}
	return dir
}

func sums(t *testing.T, dir string) string {
	var out []string
	for _, n := range requiredFiles {
		f, err := os.Open(filepath.Join(dir, n))
		if err != nil {
			t.Fatal(err)
		}
		h := sha1.New()
		io.Copy(h, f)
		f.Close()
		out = append(out, hex.EncodeToString(h.Sum(nil))+"  "+n)
	}
	sort.Strings(out)
	return strings.Join(out, "\n")
}

func TestSynthetic(t *testing.T) {
	loadCfg()
	if len(dllBytes) < 100000 || len(iniBytes) < 1000 {
		t.Fatal("payload не вшит")
	}
	dir := makeGame(t)
	var lines []string
	lg := func(ru, en string) { lines = append(lines, en) }
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	st := func(data, ai string) {
		t.Helper()
		if d, a := detectState(dir), aiState(dir); d != data || a != ai {
			t.Fatalf("state data=%s ai=%s, want %s %s", d, a, data, ai)
		}
	}
	st("original", "none")
	orig := sums(t, dir)
	all := Opts{Data: true, AI: true}

	// 1. полная установка
	must(installAll(dir, all, lg))
	st("prince", "current")
	if b, _ := os.ReadFile(filepath.Join(dir, iniName)); !bytes.Equal(b, iniBytes) {
		t.Fatal("revived.ini не записан")
	}
	prince := sums(t, dir)
	if prince == orig {
		t.Fatal("архивы не изменились")
	}
	// 2. повторная установка ничего не меняет
	must(installAll(dir, all, lg))
	if sums(t, dir) != prince {
		t.Fatal("reinstall differs")
	}
	// 3. откат = оригинал, DLL и ini убраны
	os.WriteFile(filepath.Join(dir, "revived_trace.txt"), []byte("x"), 0o644)
	must(restoreAll(dir, lg))
	st("original", "none")
	if sums(t, dir) != orig {
		t.Fatal("restore != orig")
	}
	if exists(filepath.Join(dir, iniName)) || exists(filepath.Join(dir, "revived_trace.txt")) {
		t.Fatal("ini/логи остались")
	}
	// 4. чужой dinput8.dll сохраняется и возвращается
	foreign := []byte("MZ-foreign-dll")
	os.WriteFile(filepath.Join(dir, dllName), foreign, 0o644)
	st("original", "foreign")
	must(installAll(dir, Opts{AI: true}, lg))
	st("original", "current")
	if sums(t, dir) != orig {
		t.Fatal("AI-only install changed archives")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, dllBak)); !bytes.Equal(b, foreign) {
		t.Fatal("чужой dll не сохранён")
	}
	// 5. свои настройки не затираются, сброс делает копию
	custom := append([]byte("; mine\n"), iniBytes...)
	os.WriteFile(filepath.Join(dir, iniName), custom, 0o644)
	must(installAll(dir, Opts{AI: true}, lg))
	if b, _ := os.ReadFile(filepath.Join(dir, iniName)); !bytes.Equal(b, custom) {
		t.Fatal("пользовательский ini затёрт")
	}
	must(installAll(dir, Opts{AI: true, ResetIni: true}, lg))
	if b, _ := os.ReadFile(filepath.Join(dir, iniName)); !bytes.Equal(b, iniBytes) {
		t.Fatal("сброс ini не сработал")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, iniBak)); !bytes.Equal(b, custom) {
		t.Fatal("копия ini не сохранена")
	}
	// 6. откат: чужой dll на месте, изменённый ini остаётся
	os.WriteFile(filepath.Join(dir, iniName), custom, 0o644)
	must(restoreAll(dir, lg))
	st("original", "foreign")
	if b, _ := os.ReadFile(filepath.Join(dir, dllName)); !bytes.Equal(b, foreign) {
		t.Fatal("чужой dll не возвращён")
	}
	if !exists(filepath.Join(dir, iniName)) {
		t.Fatal("изменённый ini удалён")
	}
	os.Remove(filepath.Join(dir, dllName))
	os.Remove(filepath.Join(dir, iniName))
	// 7. старая версия нашей библиотеки обновляется
	os.WriteFile(filepath.Join(dir, dllName), []byte("MZ....revived_trace.txt...."), 0o644)
	st("original", "older")
	must(installAll(dir, Opts{AI: true}, lg))
	st("original", "current")
	must(restoreAll(dir, lg))
	// 8. только тайминги; Малик; переключения
	must(installAll(dir, Opts{Data: true}, lg))
	st("prince", "none")
	must(installAll(dir, Opts{Data: true, AI: true, Malik: true}, lg))
	st("malik", "current")
	mal := sums(t, dir)
	must(installAll(dir, Opts{Data: true, Malik: false}, lg))
	st("prince", "current")
	if sums(t, dir) != prince {
		t.Fatal("switch to prince differs")
	}
	must(installAll(dir, Opts{Data: true, Malik: true}, lg))
	if sums(t, dir) != mal {
		t.Fatal("switch to malik differs")
	}
	must(restoreAll(dir, lg))
	st("original", "none")
	if sums(t, dir) != orig {
		t.Fatal("final restore != orig")
	}
	// 9. повреждённый архив: отказ и ни одного байта записи
	j := &CFG[2]
	f, _ := os.OpenFile(filepath.Join(dir, j.Archive), os.O_RDWR, 0)
	f.WriteAt([]byte{0xAA}, j.Entry+int64(j.ApplySegs[0].Start)+1)
	f.Close()
	bad := sums(t, dir)
	if err := installAll(dir, all, lg); err == nil {
		t.Fatal("повреждённый архив не отклонён")
	}
	if sums(t, dir) != bad || exists(filepath.Join(dir, dllName)) {
		t.Fatal("при отказе что-то записано")
	}
	// 10. ничего не выбрано
	if err := installAll(dir, Opts{}, lg); err == nil {
		t.Fatal("пустой выбор принят")
	}
	// 11. не папка игры
	empty := t.TempDir()
	if detectState(empty) != "bad_folder" || restoreAll(empty, lg) == nil {
		t.Fatal("bad_folder")
	}
	if !dirWritable(dir) {
		t.Fatal("dirWritable")
	}
}
