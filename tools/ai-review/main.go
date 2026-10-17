package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
)

const cliUsage = `Usage: ai-review <command> [flags]

Commands:
  hash    Print the allowlisted corpus SHA-256
  index   Build and save an index, then print its manifest
  search  Print matching complete source chunks
  review  Review a pull request using the trusted base index

Run "ai-review <command> -h" for command flags.`

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "ai-review:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) == 0 || args[0] == "-h" || args[0] == "--help" || args[0] == "help" {
		fmt.Println(cliUsage)
		return nil
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	switch args[0] {
	case "hash":
		return runHash(args[1:])
	case "index":
		return runIndex(args[1:])
	case "search":
		return runSearch(args[1:])
	case "review":
		return runReview(ctx, args[1:])
	default:
		return fmt.Errorf("unknown command %q\n%s", args[0], cliUsage)
	}
}

func parseFlags(fs *flag.FlagSet, args []string) error {
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() != 0 {
		return fmt.Errorf("unexpected positional arguments: %s", strings.Join(fs.Args(), " "))
	}
	return nil
}

func runHash(args []string) error {
	fs := flag.NewFlagSet("hash", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	root := fs.String("root", "", "trusted repository root (required)")
	if err := parseFlags(fs, args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if *root == "" {
		return errors.New("-root is required")
	}
	hash, err := CorpusHash(*root)
	if err != nil {
		return err
	}
	fmt.Println(hash)
	return nil
}

func runIndex(args []string) error {
	fs := flag.NewFlagSet("index", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	root := fs.String("root", "", "trusted repository root (required)")
	sha := fs.String("sha", "", "source revision metadata (required)")
	out := fs.String("out", "", "index JSON output path (required)")
	if err := parseFlags(fs, args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if *root == "" || *sha == "" || *out == "" {
		return errors.New("-root, -sha, and -out are required")
	}
	idx, err := BuildIndex(*root, *sha)
	if err != nil {
		return err
	}
	if err := SaveIndex(*out, idx); err != nil {
		return err
	}
	info, err := os.Stat(*out)
	if err != nil {
		return err
	}
	fmt.Printf("corpus_hash=%s source_sha=%s chunks=%d files=%d size_bytes=%d\n",
		idx.CorpusHash, idx.SourceSHA, len(idx.Chunks), len(indexFilePaths(idx)), info.Size())
	return nil
}

func indexFilePaths(idx *Index) map[string]struct{} {
	paths := make(map[string]struct{})
	for _, chunk := range idx.Chunks {
		paths[chunk.Path] = struct{}{}
	}
	return paths
}

func runSearch(args []string) error {
	fs := flag.NewFlagSet("search", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	indexPath := fs.String("index", "", "index JSON path (required)")
	query := fs.String("query", "", "lexical search query (required)")
	top := fs.Int("top", 6, "maximum matching chunks")
	if err := parseFlags(fs, args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if *indexPath == "" || strings.TrimSpace(*query) == "" {
		return errors.New("-index and -query are required")
	}
	if *top <= 0 {
		return errors.New("-top must be greater than zero")
	}
	idx, err := LoadIndex(*indexPath, "")
	if err != nil {
		return err
	}
	for _, chunk := range SearchIndex(idx, *query, *top) {
		fmt.Printf("--- %s:%d-%d (chunk %d) ---\n%s\n", strconv.QuoteToASCII(chunk.Path), chunk.StartLine, chunk.EndLine, chunk.ID, chunk.Text)
	}
	return nil
}

func runReview(ctx context.Context, args []string) error {
	fs := flag.NewFlagSet("review", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	root := fs.String("root", "", "trusted base checkout root (required)")
	indexPath := fs.String("index", "", "trusted index JSON path (required)")
	repo := fs.String("repo", "VaSeWS/iRon_fork", "GitHub repository owner/name")
	pr := fs.Int("pr", 0, "pull request number (required)")
	base := fs.String("base", "", "exact base commit SHA (required)")
	head := fs.String("head", "", "exact pull request head commit SHA (required)")
	publish := fs.Bool("publish", false, "publish or update the marked review comment")
	out := fs.String("out", "", "write review Markdown to this path")
	model := fs.String("model", defaultReviewModel, "Groq model identifier")
	if err := parseFlags(fs, args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if *root == "" || *indexPath == "" || *pr <= 0 || *base == "" || *head == "" {
		return errors.New("-root, -index, -pr, -base, and -head are required")
	}
	if strings.TrimSpace(*repo) == "" || strings.Contains(*repo, " ") {
		return errors.New("-repo must be owner/name")
	}
	if strings.TrimSpace(*model) == "" {
		return errors.New("-model must not be empty")
	}
	return ReviewPR(ctx, ReviewOptions{
		Root: *root, IndexPath: *indexPath, Repo: *repo, PR: *pr,
		BaseSHA: *base, HeadSHA: *head, Model: *model, Output: *out, Publish: *publish,
	})
}
