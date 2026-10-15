// Firmware table filled in by the generated FwData.cpp (scripts/fw_gen.py).
#ifndef QCABT_FWDATA_H
#define QCABT_FWDATA_H
#include <stddef.h>
struct qca_fw_entry { const char *name; const unsigned char *data; size_t size; };
extern const struct qca_fw_entry qca_fw_table[];
extern const int qca_fw_count;
#endif
