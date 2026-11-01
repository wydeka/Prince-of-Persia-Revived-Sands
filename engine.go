package main

import (
	"bytes"
	_ "embed"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

//go:embed cfg.json
var cfgJSON []byte

const (
	headerSizePos = 395
	headA         = "DataPC_actors.forge"
	princeOff     = 2809856
	malikOff      = 194641920
	gameExe       = "Prince of Persia.exe" // в старой версии было Prince_of_Persia.exe - такого процесса нет, и запущенная игра не замечалась
)

var requiredFiles = []string{"DataPC.forge", "DataPC_actors.forge", "DataPC_01-03.forge"}
var swaps = [][3]int{{1166, 1230, 8}, {1178, 1242, 4}, {81510, 82262, 4}}
var mp = map[int]int{1178: 1242, 1242: 1178, 81510: 82262, 82262: 81510}

// ---- конфиг ----

type Seg struct {
	Start, Length int
	Data          []byte
}

func (s *Seg) UnmarshalJSON(b []byte) error {
	var a []json.RawMessage
	if err := json.Unmarshal(b, &a); err != nil || len(a) != 3 {
		return errors.New("bad seg")
	}
	var str string
	if err := json.Unmarshal(a[0], &s.Start); err != nil {
		return err
	}
	if err := json.Unmarshal(a[1], &s.Length); err != nil {
		return err
	}
	if err := json.Unmarshal(a[2], &str); err != nil {
		return err
	}
	d, err := base64.StdEncoding.DecodeString(str)
	s.Data = d
	return err
}

type Slot struct {
	Pos      int
	Old, New []byte
}

func (s *Slot) UnmarshalJSON(b []byte) error {
	var a []json.RawMessage
	if err := json.Unmarshal(b, &a); err != nil || len(a) != 3 {
		return errors.New("bad slot")
	}
	var o, n string
	if err := json.Unmarshal(a[0], &s.Pos); err != nil {
		return err
	}
	json.Unmarshal(a[1], &o)
	json.Unmarshal(a[2], &n)
	var err error
	if s.Old, err = base64.StdEncoding.DecodeString(o); err != nil {
		return err
	}
	s.New, err = base64.StdEncoding.DecodeString(n)
	return err
}

type Job struct {
	Name         string `json:"name"`
	Archive      string `json:"archive"`
	Entry        int64  `json:"entry"`
	IndexPos     *int   `json:"index_pos"`
	TablePos     *int   `json:"table_pos"`
	Len0         int    `json:"len0"`
	Len1         int    `json:"len1"`
	Size0        int    `json:"size0"`
	Size1        int    `json:"size1"`
	ApplySegs    []Seg  `json:"apply_segs"`
	RestoreSegs  []Seg  `json:"restore_segs"`
	SlotsApply   []Slot `json:"slots_apply"`
	SlotsRestore []Slot `json:"slots_restore"`
}

var CFG []Job

func loadCfg() {
	if err := json.Unmarshal(cfgJSON, &CFG); err != nil {
		panic(err)
	}
}

func role(t *Job) string {
	if t.Archive == headA && t.Entry == princeOff {
		return "prince"
	}
	if t.Archive == headA && t.Entry == malikOff {
		return "malik"
	}
	return "common"
}

func jobsFor(malik bool) []*Job {
	var r []*Job
	for i := range CFG {
		if role(&CFG[i]) != "malik" || malik {
			r = append(r, &CFG[i])
		}
	}
	return r
}

// ---- ошибки и лог ----

type ModErr struct{ RU, EN string }

func (e *ModErr) Error() string { return e.RU + " / " + e.EN }

func merr(ru, en string, a ...any) error {
	return &ModErr{fmt.Sprintf(ru, a...), fmt.Sprintf(en, a...)}
}

type Logf func(ru, en string)

// ---- низкий уровень ----

func u32(x int) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, uint32(x))
	return b
}

func readAt(f *os.File, off int64, n int) ([]byte, error) {
	b := make([]byte, n)
	_, err := f.ReadAt(b, off)
	return b, err
}

func headState(game string) (int64, error) {
	f, err := os.Open(filepath.Join(game, headA))
	if err != nil {
		return 0, err
	}
	defer f.Close()
	b, err := readAt(f, 1166, 8)
	if err != nil {
		return 0, err
	}
	return int64(binary.LittleEndian.Uint64(b)), nil
}

func swapHead(game string) error {
	f, err := os.OpenFile(filepath.Join(game, headA), os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	for _, s := range swaps {
		x, err := readAt(f, int64(s[0]), s[2])
		if err != nil {
			return err
		}
		y, err := readAt(f, int64(s[1]), s[2])
		if err != nil {
			return err
		}
		if _, err = f.WriteAt(y, int64(s[0])); err != nil {
			return err
		}
		if _, err = f.WriteAt(x, int64(s[1])); err != nil {
			return err
		}
	}
	return nil
}

func positions(t *Job, apply, remap bool) (int, int) {
	ip, tp := -1, -1
	if t.IndexPos != nil {
		ip = *t.IndexPos
	}
	if t.TablePos != nil {
		tp = *t.TablePos
	}
	if remap && !apply && t.Archive == headA {
		ip, tp = mp[ip], mp[tp]
	}
	return ip, tp
}

func segsOf(t *Job, apply bool) (segs, other []Seg, slots []Slot) {
	if apply {
		return t.ApplySegs, t.RestoreSegs, t.SlotsApply
	}
	return t.RestoreSegs, t.ApplySegs, t.SlotsRestore
}

// quickCheck: запись находится в состоянии «до» операции (читает только нужные куски).
func quickCheck(game string, t *Job, apply, remap bool) (bool, error) {
	f, err := os.Open(filepath.Join(game, t.Archive))
	if err != nil {
		return false, err
	}
	defer f.Close()
	sizeFrom := t.Size0
	if !apply {
		sizeFrom = t.Size1
	}
	segs, other, _ := segsOf(t, apply)
	ip, tp := positions(t, apply, remap)
	b, err := readAt(f, t.Entry+headerSizePos, 4)
	if err != nil || !bytes.Equal(b, u32(sizeFrom)) {
		return false, nil
	}
	if ip >= 0 {
		s1, e1 := readAt(f, int64(ip), 4)
		s2, e2 := readAt(f, int64(tp), 4)
		if e1 != nil || e2 != nil || !bytes.Equal(s1, u32(sizeFrom)) || !bytes.Equal(s2, u32(sizeFrom)) {
			return false, nil
		}
	}
	for i, s := range segs {
		got, err := readAt(f, t.Entry+int64(s.Start), s.Length)
		if err != nil || !bytes.Equal(got, other[i].Data) {
			return false, nil
		}
	}
	return true, nil
}

func quickAll(game string, jobs []*Job, apply, remap bool) bool {
	for _, t := range jobs {
		ok, err := quickCheck(game, t, apply, remap)
		if err != nil || !ok {
			return false
		}
	}
	return true
}

// detectState: original | prince | malik | mixed | bad_folder
func detectState(game string) string {
	for _, n := range requiredFiles {
		if st, err := os.Stat(filepath.Join(game, n)); err != nil || st.IsDir() {
			return "bad_folder"
		}
	}
	head, err := headState(game)
	if err != nil {
		return "mixed"
	}
	switch head {
	case malikOff:
		if quickAll(game, jobsFor(true), false, true) {
			return "malik"
		}
	case princeOff:
		if quickAll(game, jobsFor(true), true, false) {
			return "original"
		}
		var mj *Job
		for i := range CFG {
			if role(&CFG[i]) == "malik" {
				mj = &CFG[i]
			}
		}
		ok, _ := quickCheck(game, mj, true, false)
		if ok && quickAll(game, jobsFor(false), false, false) {
			return "prince"
		}
	}
	return "mixed"
}

type prepared struct {
	path                    string
	out                     []byte
	sizeTo, lenFrom, ip, tp int
}

func prepare(game string, t *Job, apply, remap bool) (*prepared, error) {
	path := filepath.Join(game, t.Archive)
	lenFrom, lenTo := t.Len0, t.Len1
	sizeFrom, sizeTo := t.Size0, t.Size1
	if !apply {
		lenFrom, lenTo = t.Len1, t.Len0
		sizeFrom, sizeTo = t.Size1, t.Size0
	}
	segs, other, slots := segsOf(t, apply)
	ip, tp := positions(t, apply, remap)
	needRU, needEN := "нужен оригинал", "original files required"
	if !apply {
		needRU, needEN = "нужен установленный мод", "installed mod required"
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	old, err := readAt(f, t.Entry, lenFrom)
	if err != nil {
		return nil, merr("%s: не удалось прочитать архив (%s)", "%s: cannot read archive (%s)", t.Archive, err)
	}
	ok := bytes.Equal(old[headerSizePos:headerSizePos+4], u32(sizeFrom))
	if ip >= 0 {
		s1, _ := readAt(f, int64(ip), 4)
		s2, _ := readAt(f, int64(tp), 4)
		ok = ok && bytes.Equal(s1, u32(sizeFrom)) && bytes.Equal(s2, u32(sizeFrom))
	}
	if !ok {
		return nil, merr("%s: архив не в ожидаемом состоянии ("+needRU+")",
			"%s: archive is not in the expected state ("+needEN+")", t.Archive)
	}
	out := make([]byte, 0, lenTo+16)
	cur := 0
	for i, s := range segs {
		if !bytes.Equal(old[s.Start:s.Start+s.Length], other[i].Data) {
			return nil, merr("%s: кусок данных не совпал с ожидаемым ("+needRU+"). Возможно, другая версия игры.",
				"%s: a data block does not match ("+needEN+"). Possibly a different game version.", t.Archive)
		}
		out = append(out, old[cur:s.Start]...)
		out = append(out, s.Data...)
		cur = s.Start + s.Length
	}
	out = append(out, old[cur:]...)
	if len(out) != lenTo {
		return nil, merr("%s: внутренняя ошибка длины", "%s: internal length error", t.Archive)
	}
	for _, sl := range slots {
		if !bytes.Equal(out[sl.Pos:sl.Pos+2], sl.Old) {
			return nil, merr("%s: не совпал размер куска в заголовке", "%s: header slot mismatch", t.Archive)
		}
		copy(out[sl.Pos:sl.Pos+2], sl.New)
	}
	copy(out[headerSizePos:headerSizePos+4], u32(sizeTo))
	return &prepared{path, out, sizeTo, lenFrom, ip, tp}, nil
}

func writeJob(game string, t *Job, apply, remap bool) error {
	p, err := prepare(game, t, apply, remap)
	if err != nil {
		return err
	}
	f, err := os.OpenFile(p.path, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err = f.WriteAt(p.out, t.Entry); err != nil {
		return err
	}
	if len(p.out) < p.lenFrom {
		if _, err = f.WriteAt(make([]byte, p.lenFrom-len(p.out)), t.Entry+int64(len(p.out))); err != nil {
			return err
		}
	}
	if p.ip >= 0 {
		if _, err = f.WriteAt(u32(p.sizeTo), int64(p.ip)); err != nil {
			return err
		}
		if _, err = f.WriteAt(u32(p.sizeTo), int64(p.tp)); err != nil {
			return err
		}
	}
	return f.Close()
}

func checkWritable(game string, needData bool) error {
	if !dirWritable(game) {
		return &AccessErr{ModErr{
			"Нет доступа на запись в папку игры. Закройте игру и запустите установщик от имени администратора.",
			"Cannot write to the game folder. Close the game and run the installer as administrator."}}
	}
	if !needData {
		return nil
	}
	for _, n := range requiredFiles {
		f, err := os.OpenFile(filepath.Join(game, n), os.O_RDWR, 0)
		if err != nil {
			return &AccessErr{ModErr{
				fmt.Sprintf("Нет доступа на запись к %s (%v). Закройте игру и запустите установщик от имени администратора.", n, err),
				fmt.Sprintf("Cannot write to %s (%v). Close the game and run the installer as administrator.", n, err)}}
		}
		f.Close()
	}
	return nil
}

type AccessErr struct{ ModErr }

func runJobs(game string, jobs []*Job, apply, remap bool, log Logf) error {
	for i, t := range jobs {
		log(fmt.Sprintf("Проверка %d/%d: %s", i+1, len(jobs), t.Name), fmt.Sprintf("Checking %d/%d: %s", i+1, len(jobs), t.Name))
		if _, err := prepare(game, t, apply, remap); err != nil {
			return err
		}
	}
	for i, t := range jobs {
		log(fmt.Sprintf("Запись %d/%d: %s", i+1, len(jobs), t.Name), fmt.Sprintf("Writing %d/%d: %s", i+1, len(jobs), t.Name))
		if err := writeJob(game, t, apply, remap); err != nil {
			return err
		}
	}
	return nil
}

func doRestore(game string, log Logf) error {
	st := detectState(game)
	if st == "original" {
		log("Файлы уже оригинальные, возвращать нечего.", "Files are already original, nothing to restore.")
		return nil
	}
	if st != "prince" && st != "malik" {
		return merr("Файлы игры в неизвестном состоянии. Проверьте целостность файлов в Steam и повторите.",
			"Game files are in an unknown state. Verify game files in Steam and try again.")
	}
	malik := st == "malik"
	if err := runJobs(game, jobsFor(malik), false, malik, log); err != nil {
		return err
	}
	if malik {
		if err := swapHead(game); err != nil {
			return err
		}
	}
	if detectState(game) != "original" {
		return merr("После отката файлы не совпали с оригиналом. Проверьте целостность файлов в Steam.",
			"After restore the files do not match the originals. Verify game files in Steam.")
	}
	log("Файлы игры возвращены в оригинал.", "Game files restored to the originals.")
	return nil
}

func doInstall(game string, malik bool, log Logf) error {
	st := detectState(game)
	if st == "bad_folder" {
		return merr("Это не папка игры.", "This is not the game folder.")
	}
	wantNow := "prince"
	if malik {
		wantNow = "malik"
	}
	if st == wantNow {
		log("Правки таймингов уже установлены.", "The timing patch is already installed.")
		return nil
	}
	if st == "prince" || st == "malik" {
		log("Мод уже установлен - сначала возвращаю оригинал.", "The mod is already installed - restoring originals first.")
		if err := doRestore(game, log); err != nil {
			return err
		}
		st = detectState(game)
	}
	if st != "original" {
		return merr("Файлы игры не оригинальные (возможно, стоит другой патч или другая версия игры). Проверьте целостность файлов в Steam и повторите.",
			"Game files are not original (another patch or a different game version?). Verify game files in Steam and try again.")
	}
	if err := runJobs(game, jobsFor(malik), true, false, log); err != nil {
		return err
	}
	want, ruM, enM := "prince", "модель Принца", "Prince model"
	if malik {
		if err := swapHead(game); err != nil {
			return err
		}
		want, ruM, enM = "malik", "модель Малика", "Malik model"
	}
	if detectState(game) != want {
		return merr("Установка завершилась, но проверка не прошла. Нажмите «Вернуть всё назад».",
			"Install finished but verification failed. Press 'Restore everything'.")
	}
	log("Тайминги установлены ("+ruM+").", "Timing patch installed ("+enM+").")
	return nil
}
