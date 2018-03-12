package main

import (
	"os"
	fmt "github.com/jhunt/go-ansi"

	"github.com/jhunt/go-cli"
	"github.com/jhunt/go-firehose"
)

func main() {
	var opts struct {
		Config string `cli:"-c, --config"`
	}
	opts.Config = "cf-bolo-nozzle.yml"

	_, _, err := cli.Parse(&opts)
	if err != nil {
		fmt.Fprintf(os.Stderr, "@R{!!! %s}\n", err)
		os.Exit(1)
	}

	firehose.Go(NewNozzle(), opts.Config)
}
