package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func fixture(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for name, content := range files {
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

func TestCorpusHashDeterminismAndAllowlist(t *testing.T) {
	root := fixture(t, map[string]string{
		"README.md":                     "overview\n",
		"memory-bank/nested.md":         "trusted memory\n",
		"src/feature.cpp":               "int featureFlag = 1;\n",
		"tests/feature_test.hpp":        "void checks();\n",
		"src/ignored.txt":               "not indexed\n",
		"tests/ignored.cpp.backup":      "not indexed\n",
		"third_party/vendor.cpp":        "vendor secret sentinel\n",
		"secrets/private.hpp":           "private secret sentinel\n",
		"memory-bank/notes.txt":         "not indexed\n",
		"src/third_party/nested.cpp":    "nested vendor sentinel\n",
		"memory-bank/secrets/hidden.md": "nested private sentinel\n",
	})
	first, err := CorpusHash(root)
	if err != nil {
		t.Fatal(err)
	}
	second, err := CorpusHash(root)
	if err != nil {
		t.Fatal(err)
	}
	if first != second {
		t.Fatalf("hash changed without corpus changes: %s != %s", first, second)
	}
	idx, err := BuildIndex(root, "revision-one")
	if err != nil {
		t.Fatal(err)
	}
	again, err := BuildIndex(root, "revision-one")
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(idx, again) {
		t.Fatal("index build is not deterministic")
	}
	if len(first) != 64 {
		t.Fatalf("unexpected SHA-256 hash %q", first)
	}
	for _, chunk := range idx.Chunks {
		if strings.Contains(chunk.Path, "third_party") || strings.Contains(chunk.Path, "secrets") || strings.Contains(chunk.Path, "ignored") {
			t.Errorf("excluded corpus path indexed: %s", chunk.Path)
		}
	}

	withSecretChange := fixture(t, map[string]string{
		"README.md":              "overview\n",
		"memory-bank/nested.md":  "trusted memory\n",
		"src/feature.cpp":        "int featureFlag = 1;\n",
		"tests/feature_test.hpp": "void checks();\n",
		"third_party/vendor.cpp": "changed vendor\n",
		"secrets/private.hpp":    "changed secret\n",
	})
	unchanged, err := CorpusHash(withSecretChange)
	if err != nil {
		t.Fatal(err)
	}
	if first != unchanged {
		t.Fatalf("excluded files affected corpus hash: %s != %s", first, unchanged)
	}
	other, err := BuildIndex(root, "different-source-sha")
	if err != nil {
		t.Fatal(err)
	}
	if other.CorpusHash != idx.CorpusHash {
		t.Fatal("source SHA affected corpus identity")
	}
}

func TestCorpusSkipsSymlinksInvalidUTF8AndBinaryFiles(t *testing.T) {
	root := fixture(t, map[string]string{
		"src/regular.cpp": "int trusted;\n",
		"src/invalid.cpp": string([]byte{0xff, 0xfe}),
		"src/binary.cpp":  "hello\x00world\n",
		"outside.cpp":     "int linkedSecret;\n",
	})
	if err := os.Symlink(filepath.Join(root, "outside.cpp"), filepath.Join(root, "src", "linked.cpp")); err != nil {
		t.Fatal(err)
	}
	hashWithExcludedFiles, err := CorpusHash(root)
	if err != nil {
		t.Fatal(err)
	}
	control := fixture(t, map[string]string{"src/regular.cpp": "int trusted;\n"})
	controlHash, err := CorpusHash(control)
	if err != nil {
		t.Fatal(err)
	}
	if hashWithExcludedFiles != controlHash {
		t.Fatal("symlink, invalid UTF-8, or NUL-containing file affected corpus hash")
	}
	idx, err := BuildIndex(root, "base")
	if err != nil {
		t.Fatal(err)
	}
	for _, chunk := range idx.Chunks {
		if chunk.Path == "src/binary.cpp" || strings.ContainsRune(chunk.Text, '\x00') {
			t.Fatalf("binary corpus content was indexed: %#v", chunk)
		}
	}
}

func TestCorpusHashTracksDocumentationAndCode(t *testing.T) {
	root := fixture(t, map[string]string{"README.md": "intro\n", "src/a.cpp": "int a;\n"})
	before, err := CorpusHash(root)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "README.md"), []byte("changed intro\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	docHash, err := CorpusHash(root)
	if err != nil {
		t.Fatal(err)
	}
	if docHash == before {
		t.Fatal("documentation change did not affect corpus hash")
	}
	if err := os.WriteFile(filepath.Join(root, "src/a.cpp"), []byte("int b;\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	codeHash, err := CorpusHash(root)
	if err != nil {
		t.Fatal(err)
	}
	if codeHash == docHash {
		t.Fatal("code change did not affect corpus hash")
	}
}

func TestBuildIndexPreservesLineBoundedSource(t *testing.T) {
	lines := []string{"alpha α", strings.Repeat("b", 1990), "omega", ""}
	content := strings.Join(lines, "\n")
	root := fixture(t, map[string]string{"src/chunk.cpp": content})
	idx, err := BuildIndex(root, "base")
	if err != nil {
		t.Fatal(err)
	}
	if len(idx.Chunks) != 2 {
		t.Fatalf("got %d chunks, want 2", len(idx.Chunks))
	}
	var rebuilt strings.Builder
	for i, chunk := range idx.Chunks {
		if chunk.ID != i+1 {
			t.Errorf("chunk ID %d at position %d", chunk.ID, i)
		}
		if len(chunk.Text) > maxChunkBytes {
			t.Errorf("chunk has %d bytes, exceeds %d", len(chunk.Text), maxChunkBytes)
		}
		if chunk.Path != "src/chunk.cpp" {
			t.Errorf("unexpected chunk path %q", chunk.Path)
		}
		if i == 0 && (chunk.StartLine != 1 || chunk.EndLine != 2) {
			t.Errorf("first line range = %d-%d", chunk.StartLine, chunk.EndLine)
		}
		if i == 1 && (chunk.StartLine != 3 || chunk.EndLine != 3) {
			t.Errorf("second line range = %d-%d", chunk.StartLine, chunk.EndLine)
		}
		rebuilt.WriteString(chunk.Text)
	}
	if rebuilt.String() != content {
		t.Fatalf("chunk text does not reproduce source: %q != %q", rebuilt.String(), content)
	}
}

func TestBuildIndexRejectsOversizedLine(t *testing.T) {
	root := fixture(t, map[string]string{"src/long.cpp": strings.Repeat("x", maxChunkBytes+1)})
	if _, err := BuildIndex(root, "base"); err == nil {
		t.Fatalf("BuildIndex error = %v, want an explicit escaped path and line error", err)
	}
}

func TestBuildIndexRejectsOversizedCorpusFile(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "src", "large.cpp")
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Truncate(path, maxCorpusFileSize+1); err != nil {
		t.Fatal(err)
	}
	if _, err := BuildIndex(root, "base"); err == nil {
		t.Fatalf("BuildIndex error = %v, want corpus size limit", err)
	}
}

func TestLoadIndexRejectsOversizedFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "index.json")
	if err := os.WriteFile(path, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Truncate(path, maxIndexSize+1); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadIndex(path, ""); err == nil {
		t.Fatalf("LoadIndex error = %v, want index size limit", err)
	}
}

func TestSaveLoadIndexIntegrityAndStaleness(t *testing.T) {
	root := fixture(t, map[string]string{"README.md": "safe retrieval\n", "src/auth.cpp": "bool verifyToken() { return true; }\n"})
	idx, err := BuildIndex(root, "revision")
	if err != nil {
		t.Fatal(err)
	}
	indexPath := filepath.Join(t.TempDir(), "index.json")
	if err := SaveIndex(indexPath, idx); err != nil {
		t.Fatal(err)
	}
	loaded, err := LoadIndex(indexPath, idx.CorpusHash)
	if err != nil {
		t.Fatal(err)
	}
	if loaded.PayloadHash == "" || loaded.CorpusHash != idx.CorpusHash || len(loaded.Chunks) != len(idx.Chunks) || !reflect.DeepEqual(loaded.Terms, idx.Terms) {
		t.Fatalf("loaded index differs from saved index: %#v", loaded)
	}
	if got := SearchIndex(loaded, "verify_token", 1); len(got) != 1 || got[0].Path != "src/auth.cpp" {
		t.Fatalf("persisted lexical retrieval = %#v", got)
	}
	if _, err := LoadIndex(indexPath, strings.Repeat("0", 64)); err == nil {
		t.Fatal("stale corpus hash was accepted")
	}

	data, err := os.ReadFile(indexPath)
	if err != nil {
		t.Fatal(err)
	}
	var tampered map[string]any
	if err := json.Unmarshal(data, &tampered); err != nil {
		t.Fatal(err)
	}
	tampered["payload_hash"] = strings.Repeat("0", 64)
	corrupt, err := json.Marshal(tampered)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(indexPath, corrupt, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadIndex(indexPath, idx.CorpusHash); err == nil {
		t.Fatalf("digest-tampered index error = %v, want integrity failure", err)
	}

	if err := json.Unmarshal(data, &tampered); err != nil {
		t.Fatal(err)
	}
	terms := tampered["terms"].(map[string]any)
	verify := terms["verify"].(map[string]any)
	postings := verify["postings"].([]any)
	postings[0].(map[string]any)["chunk_id"] = 999
	corrupt, err = json.Marshal(tampered)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(indexPath, corrupt, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadIndex(indexPath, idx.CorpusHash); err == nil {
		t.Fatalf("tampered index error = %v, want lexical postings validation failure", err)
	}
}

func TestLoadIndexRejectsUnsupportedVersionAndStructure(t *testing.T) {
	root := fixture(t, map[string]string{"README.md": "one line\n"})
	idx, err := BuildIndex(root, "revision")
	if err != nil {
		t.Fatal(err)
	}
	idx.Version++
	path := filepath.Join(t.TempDir(), "index.json")
	if err := SaveIndex(path, idx); err == nil {
		t.Fatal("SaveIndex accepted unsupported version")
	}
	idx.Version = indexVersion
	idx.Chunks[0].Path = "third_party/poison.cpp"
	if err := SaveIndex(path, idx); err == nil {
		t.Fatal("SaveIndex accepted a non-allowlisted chunk path")
	}
	idx.Chunks[0].Path = "README.md"
	idx.Chunks[0].StartLine = 2
	idx.Chunks[0].EndLine = 2
	if err := SaveIndex(path, idx); err == nil {
		t.Fatal("SaveIndex accepted a chunk with a missing initial source line")
	}
}

func TestSearchIndexSymbolAwareDeterministicAndRelevant(t *testing.T) {
	root := fixture(t, map[string]string{
		"src/auth.cpp":    "bool verifyAccessToken() { return tokenValid; }\n",
		"src/display.cpp": "void drawOverlay() { renderFrame(); }\n",
		"README.md":       "The project renders graphics.\n",
	})
	idx, err := BuildIndex(root, "revision")
	if err != nil {
		t.Fatal(err)
	}
	first := SearchIndex(idx, "verify_access_token", 3)
	second := SearchIndex(idx, "verify_access_token", 3)
	if len(first) != 1 || first[0].Path != "src/auth.cpp" {
		t.Fatalf("symbol search result = %#v, want only src/auth.cpp", first)
	}
	if !reflect.DeepEqual(first, second) {
		t.Fatalf("search results differ across identical calls: %#v != %#v", first, second)
	}
	if got := SearchIndex(idx, "absent_symbol_xyz", 3); len(got) != 0 {
		t.Fatalf("unmatched query returned invented context: %#v", got)
	}
	if got := SearchIndex(idx, "render frame", 1); len(got) != 1 || got[0].Path != "src/display.cpp" {
		t.Fatalf("render query result = %#v", got)
	}
	if got := SearchIndex(idx, "render", 0); got != nil {
		t.Fatalf("topK=0 returned results: %#v", got)
	}
}
