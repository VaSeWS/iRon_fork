package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode"
	"unicode/utf8"
)

const (
	indexVersion      = 1
	maxChunkBytes     = 2000
	maxCorpusFileSize = 4 << 20
	maxCorpusSize     = 16 << 20
	maxIndexSize      = 32 << 20
	corpusFormat      = "iron-ai-review-corpus-v1"
	chunkingFormat    = "line-bounded-utf8-2000-v1"
	tokenFormat       = "unicode-camel-snake-v1"
)

// Chunk is a line-bounded, source-preserving excerpt from one corpus file.
type Chunk struct {
	ID        int    `json:"id"`
	Path      string `json:"path"`
	StartLine int    `json:"start_line"`
	EndLine   int    `json:"end_line"`
	Text      string `json:"text"`
}

// Index stores a versioned corpus snapshot, chunks, and persisted lexical postings.
type Index struct {
	Version     int                        `json:"version"`
	CorpusHash  string                     `json:"corpus_hash"`
	SourceSHA   string                     `json:"source_sha"`
	Chunks      []Chunk                    `json:"chunks"`
	Terms       map[string]LexicalPostings `json:"terms"`
	PayloadHash string                     `json:"payload_hash"`
}

// LexicalPostings stores a term's document frequency and per-chunk term counts.
type LexicalPostings struct {
	DocumentFrequency int       `json:"document_frequency"`
	Postings          []Posting `json:"postings"`
}

// Posting records a token frequency in one chunk.
type Posting struct {
	ChunkID   int `json:"chunk_id"`
	Frequency int `json:"frequency"`
}

type corpusFile struct {
	path    string
	content []byte
}

// CorpusHash returns a stable SHA-256 identity for the indexed allowlisted corpus.
// Source control revisions are deliberately not part of corpus identity.
func CorpusHash(root string) (string, error) {
	files, err := readCorpus(root)
	if err != nil {
		return "", err
	}
	return hashCorpus(files), nil
}

// BuildIndex reads the allowlisted corpus and produces deterministic chunks.
func BuildIndex(root, sourceSHA string) (*Index, error) {
	files, err := readCorpus(root)
	if err != nil {
		return nil, err
	}
	idx := &Index{
		Version:    indexVersion,
		CorpusHash: hashCorpus(files),
		SourceSHA:  sourceSHA,
		Chunks:     make([]Chunk, 0),
	}
	for _, file := range files {
		var text strings.Builder
		start, end := 1, 0
		lineStart, lineNumber := 0, 1
		for lineStart < len(file.content) {
			lineEnd := len(file.content)
			if newline := bytes.IndexByte(file.content[lineStart:], '\n'); newline >= 0 {
				lineEnd = lineStart + newline + 1
			}
			line := file.content[lineStart:lineEnd]
			lineBytes := len(line)
			if lineBytes > maxChunkBytes {
				return nil, fmt.Errorf("%q:%d: line is %d bytes; maximum chunk line is %d bytes", file.path, lineNumber, lineBytes, maxChunkBytes)
			}
			if text.Len() > 0 && text.Len()+lineBytes > maxChunkBytes {
				idx.Chunks = append(idx.Chunks, Chunk{ID: len(idx.Chunks) + 1, Path: file.path, StartLine: start, EndLine: end, Text: text.String()})
				text.Reset()
				start = lineNumber
			}
			_, _ = text.Write(line)
			end = lineNumber
			lineStart, lineNumber = lineEnd, lineNumber+1
		}
		if text.Len() > 0 {
			idx.Chunks = append(idx.Chunks, Chunk{ID: len(idx.Chunks) + 1, Path: file.path, StartLine: start, EndLine: end, Text: text.String()})
		}
	}
	idx.Terms = buildLexicalPostings(idx.Chunks)
	if len(idx.Chunks) == 0 {
		return nil, errors.New("allowlisted corpus contains no indexable content")
	}
	return idx, nil
}

// SaveIndex validates and writes an integrity-protected JSON index.
func SaveIndex(path string, idx *Index) error {
	if idx == nil {
		return errors.New("cannot save a nil index")
	}
	if err := validateIndex(idx, ""); err != nil {
		return fmt.Errorf("invalid index: %w", err)
	}
	copy := *idx
	copy.PayloadHash = payloadHash(idx)
	data, err := json.MarshalIndent(copy, "", "  ")
	if err != nil {
		return fmt.Errorf("encode index: %w", err)
	}
	data = append(data, '\n')
	if len(data) > maxIndexSize {
		return fmt.Errorf("encoded index exceeds %d-byte limit", maxIndexSize)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		return fmt.Errorf("write index: %w", err)
	}
	return nil
}

// LoadIndex reads an index, verifies its payload integrity, and requires the
// requested corpus hash to match when expectedHash is nonempty.
func LoadIndex(path, expectedHash string) (*Index, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open index: %w", err)
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, fmt.Errorf("stat index: %w", err)
	}
	if info.Size() > maxIndexSize {
		return nil, fmt.Errorf("index exceeds %d-byte limit", maxIndexSize)
	}
	data, err := io.ReadAll(io.LimitReader(file, maxIndexSize+1))
	if err != nil {
		return nil, fmt.Errorf("read index: %w", err)
	}
	if len(data) > maxIndexSize {
		return nil, fmt.Errorf("index exceeds %d-byte limit", maxIndexSize)
	}
	var idx Index
	if err := json.Unmarshal(data, &idx); err != nil {
		return nil, fmt.Errorf("decode index: %w", err)
	}
	if err := validateIndex(&idx, expectedHash); err != nil {
		return nil, err
	}
	if idx.PayloadHash != payloadHash(&idx) {
		return nil, errors.New("index payload integrity check failed")
	}
	return &idx, nil
}

// SearchIndex returns at most topK distinct matching chunks in deterministic
// lexical relevance order. Queries with no corpus matches return no chunks.
func SearchIndex(idx *Index, query string, topK int) []Chunk {
	if idx == nil || topK <= 0 || len(idx.Chunks) == 0 {
		return nil
	}
	queryTokens := tokenSet(query)
	if len(queryTokens) == 0 {
		return nil
	}
	queryTerms := make([]string, 0, len(queryTokens))
	for term := range queryTokens {
		queryTerms = append(queryTerms, term)
	}
	sort.Strings(queryTerms)

	scores := make([]float64, len(idx.Chunks))
	for _, term := range queryTerms {
		lexical, found := idx.Terms[term]
		if !found || lexical.DocumentFrequency == 0 {
			continue
		}
		idf := math.Log1p((float64(len(idx.Chunks)-lexical.DocumentFrequency) + 0.5) / (float64(lexical.DocumentFrequency) + 0.5))
		for _, posting := range lexical.Postings {
			chunkIndex := posting.ChunkID - 1
			if chunkIndex < 0 || chunkIndex >= len(scores) || posting.Frequency <= 0 {
				continue
			}
			scores[chunkIndex] += idf * float64(posting.Frequency) / float64(posting.Frequency+1)
		}
	}

	type result struct {
		chunk Chunk
		score float64
	}
	results := make([]result, 0, len(idx.Chunks))
	seenIDs := make(map[int]struct{}, len(idx.Chunks))
	for i, chunk := range idx.Chunks {
		if scores[i] <= 0 {
			continue
		}
		if _, duplicate := seenIDs[chunk.ID]; duplicate {
			continue
		}
		seenIDs[chunk.ID] = struct{}{}
		results = append(results, result{chunk: chunk, score: scores[i]})
	}
	sort.Slice(results, func(i, j int) bool {
		if results[i].score != results[j].score {
			return results[i].score > results[j].score
		}
		if results[i].chunk.Path != results[j].chunk.Path {
			return results[i].chunk.Path < results[j].chunk.Path
		}
		if results[i].chunk.StartLine != results[j].chunk.StartLine {
			return results[i].chunk.StartLine < results[j].chunk.StartLine
		}
		if results[i].chunk.EndLine != results[j].chunk.EndLine {
			return results[i].chunk.EndLine < results[j].chunk.EndLine
		}
		return results[i].chunk.ID < results[j].chunk.ID
	})
	if len(results) > topK {
		results = results[:topK]
	}
	selected := make([]Chunk, len(results))
	for i := range results {
		selected[i] = results[i].chunk
	}
	return selected
}

func readCorpus(root string) ([]corpusFile, error) {
	absRoot, err := filepath.Abs(root)
	if err != nil {
		return nil, fmt.Errorf("resolve corpus root: %q", err.Error())
	}
	info, err := os.Stat(absRoot)
	if err != nil {
		return nil, fmt.Errorf("open corpus root %q: %q", root, err.Error())
	}
	if !info.IsDir() {
		return nil, fmt.Errorf("corpus root %q is not a directory", root)
	}
	files := make([]corpusFile, 0)
	var totalBytes int64
	addFile := func(path string, d fs.DirEntry) error {
		if d.Type()&fs.ModeSymlink != 0 || !d.Type().IsRegular() {
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return fmt.Errorf("stat corpus file %q: %q", path, err.Error())
		}
		if info.Size() > maxCorpusFileSize {
			return fmt.Errorf("corpus file %q exceeds %d-byte limit", path, maxCorpusFileSize)
		}
		file, err := os.Open(path)
		if err != nil {
			return fmt.Errorf("open corpus file %q: %q", path, err.Error())
		}
		data, readErr := io.ReadAll(io.LimitReader(file, maxCorpusFileSize+1))
		closeErr := file.Close()
		if readErr != nil {
			return fmt.Errorf("read corpus file %q: %q", path, readErr.Error())
		}
		if closeErr != nil {
			return fmt.Errorf("close corpus file %q: %q", path, closeErr.Error())
		}
		if int64(len(data)) > maxCorpusFileSize {
			return fmt.Errorf("corpus file %q exceeds %d-byte limit", path, maxCorpusFileSize)
		}
		if totalBytes > maxCorpusSize-int64(len(data)) {
			return fmt.Errorf("corpus exceeds %d-byte limit", maxCorpusSize)
		}
		totalBytes += int64(len(data))
		if !utf8.Valid(data) || bytes.IndexByte(data, 0) >= 0 {
			return nil
		}
		rel, err := filepath.Rel(absRoot, path)
		if err != nil {
			return fmt.Errorf("make corpus path relative for %q: %q", path, err.Error())
		}
		files = append(files, corpusFile{path: filepath.ToSlash(rel), content: data})
		return nil
	}
	readSingle := func(name string) error {
		path := filepath.Join(absRoot, name)
		d, err := os.Lstat(path)
		if errors.Is(err, fs.ErrNotExist) {
			return nil
		}
		if err != nil {
			return fmt.Errorf("stat corpus file %q: %q", path, err.Error())
		}
		return addFile(path, fs.FileInfoToDirEntry(d))
	}
	if err := readSingle("README.md"); err != nil {
		return nil, err
	}
	for _, dirName := range []string{"memory-bank", "src", "tests"} {
		rootDir := filepath.Join(absRoot, dirName)
		d, err := os.Lstat(rootDir)
		if errors.Is(err, fs.ErrNotExist) {
			continue
		}
		if err != nil {
			return nil, fmt.Errorf("stat corpus directory %q: %q", rootDir, err.Error())
		}
		if !d.IsDir() || d.Mode()&fs.ModeSymlink != 0 {
			continue
		}
		err = filepath.WalkDir(rootDir, func(path string, entry fs.DirEntry, walkErr error) error {
			if walkErr != nil {
				return fmt.Errorf("walk corpus entry: %q", walkErr.Error())
			}
			if entry.Type()&fs.ModeSymlink != 0 {
				if entry.IsDir() {
					return filepath.SkipDir
				}
				return nil
			}
			if entry.IsDir() {
				if entry.Name() == "third_party" || entry.Name() == "secrets" {
					return filepath.SkipDir
				}
				return nil
			}
			ext := filepath.Ext(entry.Name())
			allowed := dirName == "memory-bank" && ext == ".md" || (dirName == "src" || dirName == "tests") && (ext == ".cpp" || ext == ".h" || ext == ".hpp")
			if !allowed {
				return nil
			}
			return addFile(path, entry)
		})
		if err != nil {
			return nil, fmt.Errorf("walk corpus directory %q: %w", rootDir, err)
		}
	}
	sort.Slice(files, func(i, j int) bool { return files[i].path < files[j].path })
	return files, nil
}

func hashCorpus(files []corpusFile) string {
	h := sha256.New()
	writeHashPart(h, []byte(corpusFormat))
	writeHashPart(h, []byte(chunkingFormat))
	writeHashPart(h, []byte(tokenFormat))
	for _, file := range files {
		writeHashPart(h, []byte(file.path))
		writeHashPart(h, file.content)
	}
	return hex.EncodeToString(h.Sum(nil))
}

type hashWriter interface {
	Write([]byte) (int, error)
}

func writeHashPart(h hashWriter, data []byte) {
	var length [8]byte
	for i := range length {
		length[7-i] = byte(uint64(len(data)) >> (i * 8))
	}
	_, _ = h.Write(length[:])
	_, _ = h.Write(data)
}

type indexPayload struct {
	Version    int                        `json:"version"`
	CorpusHash string                     `json:"corpus_hash"`
	SourceSHA  string                     `json:"source_sha"`
	Chunks     []Chunk                    `json:"chunks"`
	Terms      map[string]LexicalPostings `json:"terms"`
}

func payloadHash(idx *Index) string {
	payload := indexPayload{
		Version: idx.Version, CorpusHash: idx.CorpusHash, SourceSHA: idx.SourceSHA,
		Chunks: idx.Chunks, Terms: idx.Terms,
	}
	data, _ := json.Marshal(payload)
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func validateIndex(idx *Index, expectedHash string) error {
	if idx.Version != indexVersion {
		return fmt.Errorf("unsupported index version %d", idx.Version)
	}
	if !isSHA256(idx.CorpusHash) {
		return errors.New("index corpus_hash must be a lowercase SHA-256 hex digest")
	}
	if expectedHash != "" && idx.CorpusHash != expectedHash {
		return fmt.Errorf("index corpus hash mismatch: expected %s, got %s", expectedHash, idx.CorpusHash)
	}
	if len(idx.Chunks) == 0 {
		return errors.New("index contains no chunks")
	}
	lastPath, lastEnd := "", 0
	for i, chunk := range idx.Chunks {
		if chunk.ID != i+1 {
			return fmt.Errorf("chunk ID %d is not the expected sequential ID %d", chunk.ID, i+1)
		}
		if !allowedCorpusPath(chunk.Path) || filepath.ToSlash(filepath.Clean(filepath.FromSlash(chunk.Path))) != chunk.Path {
			return fmt.Errorf("chunk %d has invalid corpus path %q", chunk.ID, chunk.Path)
		}
		if chunk.StartLine < 1 || chunk.EndLine < chunk.StartLine {
			return fmt.Errorf("chunk %d has invalid line range %d-%d", chunk.ID, chunk.StartLine, chunk.EndLine)
		}
		if lastPath > chunk.Path || lastPath == chunk.Path && chunk.StartLine != lastEnd+1 || lastPath != chunk.Path && chunk.StartLine != 1 {
			return fmt.Errorf("chunk %d is not in contiguous corpus order", chunk.ID)
		}
		if !utf8.ValidString(chunk.Text) || strings.IndexByte(chunk.Text, '\x00') >= 0 || chunk.Text == "" || len(chunk.Text) > maxChunkBytes {
			return fmt.Errorf("chunk %d text is empty, binary, invalid UTF-8, or exceeds %d bytes", chunk.ID, maxChunkBytes)
		}

		lineCount := strings.Count(chunk.Text, "\n")
		if !strings.HasSuffix(chunk.Text, "\n") {
			lineCount++
		}
		if lineCount != chunk.EndLine-chunk.StartLine+1 {
			return fmt.Errorf("chunk %d text line count does not match its metadata", chunk.ID)
		}
		lastPath, lastEnd = chunk.Path, chunk.EndLine
	}
	if len(idx.Terms) == 0 {
		return errors.New("index contains no lexical postings")
	}
	seenChunks := make([]bool, len(idx.Chunks))
	for term, lexical := range idx.Terms {
		normalizedCounts := termCounts(term)
		if len(normalizedCounts) != 1 || normalizedCounts[term] != 1 {
			return fmt.Errorf("index contains invalid normalized term %q", term)
		}
		if lexical.DocumentFrequency < 1 || lexical.DocumentFrequency > len(idx.Chunks) || lexical.DocumentFrequency != len(lexical.Postings) {
			return fmt.Errorf("term %q has invalid document frequency", term)
		}
		lastID := 0
		for _, posting := range lexical.Postings {
			if posting.ChunkID <= lastID || posting.ChunkID > len(idx.Chunks) {
				return fmt.Errorf("term %q has invalid or unordered posting chunk ID %d", term, posting.ChunkID)
			}
			chunk := idx.Chunks[posting.ChunkID-1]
			if posting.Frequency < 1 || posting.Frequency > len(chunk.Text)+len(chunk.Path) {
				return fmt.Errorf("term %q has invalid frequency for chunk %d", term, posting.ChunkID)
			}
			seenChunks[posting.ChunkID-1] = true
			lastID = posting.ChunkID
		}
	}
	for i, found := range seenChunks {
		if !found {
			return fmt.Errorf("chunk %d has no lexical postings", i+1)
		}
	}
	return nil
}

func buildLexicalPostings(chunks []Chunk) map[string]LexicalPostings {
	terms := make(map[string]LexicalPostings)
	for _, chunk := range chunks {
		counts := termCounts(chunk.Text + " " + chunk.Path)
		for term, frequency := range counts {
			list := terms[term]
			list.Postings = append(list.Postings, Posting{ChunkID: chunk.ID, Frequency: frequency})
			list.DocumentFrequency++
			terms[term] = list
		}
	}
	return terms
}

func allowedCorpusPath(path string) bool {
	if path == "README.md" {
		return true
	}
	if !strings.HasPrefix(path, "memory-bank/") && !strings.HasPrefix(path, "src/") && !strings.HasPrefix(path, "tests/") {
		return false
	}
	for _, part := range strings.Split(path, "/") {
		if part == "third_party" || part == "secrets" {
			return false
		}
	}
	if strings.HasPrefix(path, "memory-bank/") {
		return filepath.Ext(path) == ".md"
	}
	ext := filepath.Ext(path)
	return ext == ".cpp" || ext == ".h" || ext == ".hpp"
}

func isSHA256(value string) bool {
	if len(value) != sha256.Size*2 {
		return false
	}
	for _, c := range value {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func tokenSet(text string) map[string]struct{} {
	counts := termCounts(text)
	set := make(map[string]struct{}, len(counts))
	for term := range counts {
		set[term] = struct{}{}
	}
	return set
}

func termCounts(text string) map[string]int {
	counts := make(map[string]int)
	var token []rune
	flush := func() {
		if len(token) > 0 {
			counts[string(token)]++
			token = token[:0]
		}
	}
	runes := []rune(text)
	for i, current := range runes {
		if !unicode.IsLetter(current) && !unicode.IsDigit(current) {
			flush()
			continue
		}
		if len(token) > 0 {
			previous := runes[i-1]
			nextLower := i+1 < len(runes) && unicode.IsLower(runes[i+1])
			if unicode.IsUpper(current) && (unicode.IsLower(previous) || unicode.IsDigit(previous) || unicode.IsUpper(previous) && nextLower) {
				flush()
			}
		}
		token = append(token, unicode.ToLower(current))
	}
	flush()
	return counts
}
