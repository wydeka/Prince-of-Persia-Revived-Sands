package main

import (
	"crypto/sha1"
	"encoding/hex"
	"io"
	"os"
	"path/filepath"
	"sort"
	"testing"
)

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
	s := ""
	for _, l := range out {
		s += l + "\n"
	}
	return s
}

func TestScenario(t *testing.T) {
	dir := os.Getenv("POP_TEST_DIR")
	if dir == "" {
		t.Skip("POP_TEST_DIR not set")
	}
	loadCfg()
	var keep []Job
	for _, j := range CFG {
		if os.Getenv("POP_ALL") == "1" || j.Archive != "DataPC_01-03.forge" || j.Entry == 569851904 {
			keep = append(keep, j)
		}
	}
	CFG = keep
	lg := func(ru, en string) {}
	orig := sums(t, dir)
	must := func(err error) {
		if err != nil {
			t.Fatal(err)
		}
	}
	st := func(want string) {
		if g := detectState(dir); g != want {
			t.Fatalf("state %s want %s", g, want)
		}
	}
	st("original")
	must(doInstall(dir, false, lg))
	st("prince")
	prince := sums(t, dir)
	must(doInstall(dir, false, lg))
	if sums(t, dir) != prince {
		t.Fatal("reinstall differs")
	}
	must(doRestore(dir, lg))
	st("original")
	if sums(t, dir) != orig {
		t.Fatal("restore != orig")
	}
	must(doInstall(dir, true, lg))
	st("malik")
	mal := sums(t, dir)
	if want := os.Getenv("POP_MALIK_SUMS"); want != "" {
		b, _ := os.ReadFile(want)
		if string(b) != mal {
			t.Fatalf("malik hashes differ from python AW:\n%s\nvs\n%s", mal, b)
		}
	}
	must(doInstall(dir, false, lg))
	if sums(t, dir) != prince {
		t.Fatal("switch to prince differs")
	}
	must(doInstall(dir, true, lg))
	if sums(t, dir) != mal {
		t.Fatal("switch to malik differs")
	}
	must(doRestore(dir, lg))
	if sums(t, dir) != orig {
		t.Fatal("final restore != orig")
	}
	if os.Getenv("POP_LEAVE") == "1" {
		must(doInstall(dir, false, lg))
	}
	// откат мода, установленного прежней сборкой (куски из alt_apply)
	if os.Getenv("POP_ALL") == "1" {
		must(doInstall(dir, false, lg))
		n := 0
		for i := range CFG {
			j := &CFG[i]
			if len(j.AltApply) == 0 {
				continue
			}
			f, err := os.OpenFile(filepath.Join(dir, j.Archive), os.O_RDWR, 0)
			must(err)
			for _, a := range j.AltApply {
				_, err = f.WriteAt(a.Data, j.Entry+int64(a.Start))
				must(err)
				n++
			}
			f.Close()
		}
		if n == 0 {
			t.Fatal("no alt segments")
		}
		st("prince")
		must(doRestore(dir, lg))
		st("original")
		if sums(t, dir) != orig {
			t.Fatal("legacy restore != orig")
		}
	}
	// повреждённый файл: отказ без записи
	j := CFG[len(CFG)-2]
	_ = j
}
