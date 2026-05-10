#ifndef DSVIEW_MCP_DECODE_H
#define DSVIEW_MCP_DECODE_H

int cmd_list_decoders(const char *decoders_dir);

/* `stack_csv` is a comma-separated list of upper-layer protocol ids
 * stacked on top of the base `protocol`, e.g. "24xx" stacked on i2c
 * exposes 24xx EEPROM operations / address / data. Each element may
 * include per-stack options after a '|' separator: "24xx|chip=24c02".
 * NULL or empty disables stacking. */
int cmd_decode(const char *input_prefix,
               const char *protocol,
               const char *map_csv,
               const char *options_csv,
               const char *stack_csv,
               const char *decoders_dir,
               long long start_sample,
               long long end_sample,
               int max_annotations);

#endif
