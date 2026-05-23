#ifndef CMD_HANDLER_H
#define CMD_HANDLER_H

#include "frame_protocol.h"

/* 分发一帧到对应的命令处理器。
 * 同时更新心跳时间戳和恢复标志。
 */
void dispatch_frame(const ParsedFrame_t *f);

#endif /* CMD_HANDLER_H */
