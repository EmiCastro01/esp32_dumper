# Human Ideas

These are human ideas that were either part of an agent idea or were iterated with.

## CLI

The project should offer the usage of a Command Line Interface that allows the user do some of the following operations:

- esp32dumper dump: Run the main script which will start the protocol communication between the host and the device
- esp32dumper dump --flash-size <S>: Allow the manual size of the dump.- It must depend on the flash, so if the user misconfigure this parameter, the system should show an error.
- esp32dumper dump --force-dump --flash-size <S>: It will ignore all sanity checks and just dump all the chunks the user said.
- esp32dumper doctor: should test if all the needed dependencies are well installed and have the correct versions.
- esp32dumper dump --debug: This is experimental: it allows pause the protocol to analyze chunk by chunk. I don't really know why this could be useful but sounds interesting.
- esp32dumper use-protocol <I|R>: Allows the user send a single protocol command.
- esp32dumper dump --out-dir <D>: store the dump in a diferent directory (default is dumps/)
- esp32dumper dump --out-format=<e|r|v>: stores the dump either extracted (by unblob or binwalk), raw (just the binary), or verbose (binary + protocol logs)
- esp32dumper dump --verbose: Print all communication logs
- esp32dumper set [board|chip|freq|backend]: Updates the configuration files


