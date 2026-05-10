#ifndef DSVIEW_MCP_DECODE_H
#define DSVIEW_MCP_DECODE_H

int cmd_list_decoders(const char *decoders_dir);
int cmd_decode(const char *input_prefix,
               const char *protocol,
               const char *map_csv,
               const char *options_csv,
               const char *decoders_dir,
               long long start_sample,
               long long end_sample,
               int max_annotations);

#endif
