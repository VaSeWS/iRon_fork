package main

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestValidateReviewResult(t *testing.T) {
	files := []changedFile{{Path: "src/a.go", Text: "one\ntwo\n", Lines: 2}, {Path: "gone.go", OldPath: "gone.go", Deleted: true, Text: "old\n", Lines: 1}, {Path: "asset.bin", Binary: true}}
	chunks := []Chunk{{ID: 4}}
	base := reviewResult{Bugs: []reviewFinding{{Path: "src/a.go", Line: 2, Severity: "high", Title: "Issue", Reason: "Reason", Fix: "Fix", SourceIDs: []int{4}}}}
	cases := []struct {
		name    string
		result  reviewResult
		wantErr bool
	}{
		{"valid", base, false},
		{"line out of bounds", reviewResult{Bugs: []reviewFinding{{Path: "src/a.go", Line: 3, Severity: "low", Title: "x", Reason: "x", Fix: "x"}}}, true},
		{"unknown path", reviewResult{Bugs: []reviewFinding{{Path: "other.go", Line: 1, Severity: "low", Title: "x", Reason: "x", Fix: "x"}}}, true},
		{"deleted file location", reviewResult{Bugs: []reviewFinding{{Path: "gone.go", Line: 1, Severity: "medium", Title: "x", Reason: "x", Fix: "x"}}}, false},
		{"binary location", reviewResult{Bugs: []reviewFinding{{Path: "asset.bin", Line: 1, Severity: "low", Title: "x", Reason: "x", Fix: "x"}}}, true},
		{"severity", reviewResult{Bugs: []reviewFinding{{Path: "src/a.go", Line: 1, Severity: "urgent", Title: "x", Reason: "x", Fix: "x"}}}, true},
		{"unknown source", reviewResult{Bugs: []reviewFinding{{Path: "src/a.go", Line: 1, Severity: "low", Title: "x", Reason: "x", Fix: "x", SourceIDs: []int{9}}}}, true},
		{"duplicate source", reviewResult{Bugs: []reviewFinding{{Path: "src/a.go", Line: 1, Severity: "low", Title: "x", Reason: "x", Fix: "x", SourceIDs: []int{4, 4}}}}, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := validateReviewResult(tc.result, files, chunks)
			if (err != nil) != tc.wantErr {
				t.Fatalf("validateReviewResult() error = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}
func TestDecodeReviewResultRequiresThreePresentArrays(t *testing.T) {
	cases := []struct {
		name string
		body string
		ok   bool
	}{
		{"all arrays", `{"bugs":[],"architecture":[],"recommendations":[]}`, true},
		{"missing category", `{"bugs":[],"architecture":[]}`, false},
		{"null category", `{"bugs":[],"architecture":null,"recommendations":[]}`, false},
		{"unknown category", `{"bugs":[],"architecture":[],"recommendations":[],"other":[]}`, false},
		{"unknown finding field", `{"bugs":[{"path":"a","line":1,"severity":"low","title":"x","reason":"x","fix":"x","source_ids":[],"extra":true}],"architecture":[],"recommendations":[]}`, false},
		{"missing source ids", `{"bugs":[{"path":"a","line":1,"severity":"low","title":"x","reason":"x","fix":"x"}],"architecture":[],"recommendations":[]}`, false},
		{"null source ids", `{"bugs":[{"path":"a","line":1,"severity":"low","title":"x","reason":"x","fix":"x","source_ids":null}],"architecture":[],"recommendations":[]}`, false},
		{"empty source ids", `{"bugs":[{"path":"a","line":1,"severity":"low","title":"x","reason":"x","fix":"x","source_ids":[]}],"architecture":[],"recommendations":[]}`, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := decodeReviewResult(tc.body)
			if (err == nil) != tc.ok {
				t.Fatalf("decodeReviewResult() error = %v, want success %v", err, tc.ok)
			}
		})
	}
}

func TestLineCountDoesNotAddTrailingNewlineLine(t *testing.T) {
	for input, want := range map[string]int{"": 0, "one": 1, "one\n": 1, "one\ntwo\n": 2} {
		if got := lineCount(input); got != want {
			t.Errorf("lineCount(%q) = %d, want %d", input, got, want)
		}
	}
}

func TestTrustedActionsBotTokenRequiresActionsTokenMatch(t *testing.T) {
	for _, tc := range []struct {
		name, actions, token, workflowToken string
		want                                bool
	}{
		{"local token", "", "pat", "", false},
		{"actions without workflow token", "true", "pat", "", false},
		{"actions with another token", "true", "pat", "other", false},
		{"matching actions token", "true", "workflow-token", "workflow-token", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv("GITHUB_ACTIONS", tc.actions)
			t.Setenv("GITHUB_TOKEN", tc.workflowToken)
			if got := trustedActionsBotToken(tc.token); got != tc.want {
				t.Fatalf("trustedActionsBotToken() = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestReviewSafetyHelpers(t *testing.T) {
	if containsCredential("safe response", "secret-value") {
		t.Fatal("false credential reflection detected")
	}
	if !containsCredential(`{"reason":"secret-value"}`, "secret-value") {
		t.Fatal("reflected credential was not detected")
	}
	var b limitedBuffer
	if _, err := b.Write(make([]byte, maxGitOutput)); err != nil {
		t.Fatal(err)
	}
	if _, err := b.Write([]byte{1}); err == nil {
		t.Fatal("git output limit was not enforced")
	}
}

func TestDiverseChunksPrefersDistinctPaths(t *testing.T) {
	got := diverseChunks([]Chunk{{ID: 1, Path: "a.md"}, {ID: 2, Path: "a.md"}, {ID: 3, Path: "b.go"}}, 2)
	if len(got) != 2 || got[0].Path != "a.md" || got[1].Path != "b.go" {
		t.Fatalf("diverseChunks() = %#v", got)
	}
}
func TestDiverseChunksRetainsDocumentationAndCode(t *testing.T) {
	ranked := []Chunk{
		{ID: 1, Path: "src/unrelated.cpp"},
		{ID: 2, Path: "memory-bank/review-rules.md"},
		{ID: 3, Path: "src/delta_logic.cpp"},
		{ID: 4, Path: "tests/delta_test.cpp"},
		{ID: 5, Path: "README.md"},
	}
	got := diverseChunks(ranked, 4)
	if len(got) != 4 {
		t.Fatalf("diverseChunks() returned %d chunks, want 4", len(got))
	}
	hasDocs, hasCode := false, false
	for _, chunk := range got {
		hasDocs = hasDocs || strings.HasPrefix(chunk.Path, "memory-bank/")
		hasCode = hasCode || strings.HasPrefix(chunk.Path, "src/") || strings.HasPrefix(chunk.Path, "tests/")
	}
	if !hasDocs || !hasCode {
		t.Fatalf("context omitted related documentation or code: %#v", got)
	}
}

func TestRenderDeletedFindingUsesBaseLink(t *testing.T) {
	opts := ReviewOptions{Repo: "VaSeWS/iRon_fork", BaseSHA: strings.Repeat("a", 40), HeadSHA: strings.Repeat("b", 40)}
	result := reviewResult{Bugs: []reviewFinding{{Path: "gone.go", Line: 1, Severity: "high", Title: "Issue", Reason: "Reason", Fix: "Fix"}}}
	got := renderReview(opts, result, []changedFile{{Path: "gone.go", OldPath: "gone.go", Deleted: true, Lines: 1}}, nil, nil)
	if !strings.Contains(got, "/blob/"+opts.BaseSHA+"/gone.go#L1") {
		t.Fatalf("deleted finding does not cite base revision: %s", got)
	}
	if strings.Contains(got, "/blob/"+opts.HeadSHA+"/gone.go#L1") {
		t.Fatalf("deleted finding incorrectly cites head: %s", got)
	}
}
func TestRenderRenamedFindingUsesNewHeadPath(t *testing.T) {
	opts := ReviewOptions{Repo: "VaSeWS/iRon_fork", BaseSHA: strings.Repeat("a", 40), HeadSHA: strings.Repeat("b", 40)}
	result := reviewResult{Bugs: []reviewFinding{{Path: "new name.go", Line: 1, Severity: "high", Title: "Issue", Reason: "Reason", Fix: "Fix"}}}
	files := []changedFile{{OldPath: "old name.go", Path: "new name.go", Status: "R100", Lines: 1}}
	got := renderReview(opts, result, files, nil, nil)
	if !strings.Contains(got, "/blob/"+opts.HeadSHA+"/new%20name.go#L1") {
		t.Fatalf("renamed finding does not cite new head path: %s", got)
	}
	if strings.Contains(got, "/blob/"+opts.BaseSHA+"/old%20name.go#L1") {
		t.Fatalf("renamed finding incorrectly cites old base path: %s", got)
	}
}

func TestReadChangedFileKinds(t *testing.T) {
	root := t.TempDir()
	run := func(args ...string) {
		t.Helper()
		cmd := exec.Command("git", append([]string{"-C", root}, args...)...)
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v: %s", args, err, out)
		}
	}
	run("init", "-q")
	run("config", "user.email", "test@example.invalid")
	run("config", "user.name", "Test")
	write := func(name, content string) {
		t.Helper()
		p := filepath.Join(root, name)
		if err := os.WriteFile(p, []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	write("old.txt", "removed\n")
	write("rename.txt", "same\n")
	write("binary.bin", "one\x00two")
	run("add", ".")
	run("commit", "-qm", "base")
	base := gitTestSHA(t, root)
	if err := os.Symlink("target", filepath.Join(root, "link")); err != nil {
		t.Fatal(err)
	}
	write("rename-new.txt", "same\n")
	run("rm", "old.txt", "rename.txt")
	write("binary.bin", strings.Repeat("x\x00", 35<<10))
	write("-odd\nname.go", "odd path\n")
	write("new.txt", "added\n")
	run("add", ".")
	run("commit", "-qm", "head")
	head := gitTestSHA(t, root)
	files, coverage, diff, err := readChangedFiles(context.Background(), root, base, head)
	if err != nil {
		t.Fatal(err)
	}
	seen := map[string]changedFile{}
	for _, f := range files {
		seen[f.Path] = f
	}
	if !seen["old.txt"].Deleted || seen["old.txt"].Status != "D" || seen["new.txt"].Deleted || seen["new.txt"].OldPath != "" {
		t.Fatal("add/delete status or sides not identified")
	}
	if seen["-odd\nname.go"].Text != "odd path\n" {
		t.Fatal("path with leading dash/newline was not read literally")
	}
	if !strings.Contains(diff, "+added") || !strings.Contains(diff, "diff --git") || !strings.Contains(diff, "Binary files") {
		t.Fatal("text changes or binary marker were missing from ordinary diff")
	}
	if seen["rename-new.txt"].Deleted || seen["rename-new.txt"].OldPath != "rename.txt" {
		t.Fatal("rename endpoints not identified")
	}
	if !seen["binary.bin"].Binary {
		t.Fatal("binary modification not identified")
	}
	if !seen["link"].Symlink {
		t.Fatal("symlink change was not identified")
	}
	if len(coverage) == 0 {
		t.Fatal("binary omission not reported")
	}
}

func gitTestSHA(t *testing.T, root string) string {
	t.Helper()
	out, err := exec.Command("git", "-C", root, "rev-parse", "HEAD").Output()
	if err != nil {
		t.Fatal(err)
	}
	return strings.TrimSpace(string(out))
}
