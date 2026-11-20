#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/*************************************************
函数名称 : RTSPServer_InitLib
函数功能 : 初始化库。进程内调用一次。
输入参数说明 : dbgLevel 非 0 时向 stderr 打一行一个事件的日志。
              minPort、maxPort 是 UDP 的 server_port 池，只分配偶数端口。
函数返回值的说明 : 成功返回 0，失败返回 -1。
*************************************************/
int RTSPServer_InitLib(int dbgLevel, unsigned short minPort, unsigned short maxPort);

/*************************************************
函数名称 : RTSPServer_FiniLib
函数功能 : 停掉还在跑的服务并释放端口池。
函数返回值的说明 : 成功返回 0，失败返回 -1。
*************************************************/
int RTSPServer_FiniLib(void);

/*************************************************
函数名称 : RTSPServer_Create
函数功能 : 创建一台服务，此时还不监听。
输入参数说明 : listenPort 为 RTSP 监听端口。
函数返回值的说明 : 成功返回服务句柄，失败返回 -1。
*************************************************/
int RTSPServer_Create(unsigned short listenPort);

/*************************************************
函数名称 : RTSPServer_Destroy
函数功能 : 销毁服务。若仍在监听，会先停掉。
输入参数说明 : hServer 为服务句柄。
函数返回值的说明 : 成功返回 0，失败返回 -1。
*************************************************/
int RTSPServer_Destroy(int hServer);

/*************************************************
函数名称 : RTSPServer_AddFileStream
函数功能 : 挂一路 Annex-B H.264 文件。播完从头循环。
输入参数说明 : name 是 URL 路径，例如 "live"。
              path 是裸流文件。fps 小于 1 时按 25。
函数返回值的说明 : 成功返回 0，失败返回 -1。
*************************************************/
int RTSPServer_AddFileStream(int hServer, const char *name, const char *path, int fps);

/*************************************************
函数名称 : RTSPServer_AddStream
函数功能 : 挂一路由调用方送帧的流。
输入参数说明 : name 是 URL 路径。fps 目前只作记录。
函数返回值的说明 : 成功返回 0，失败返回 -1。
*************************************************/
int RTSPServer_AddStream(int hServer, const char *name, int fps);

/*************************************************
函数名称 : RTSPServer_InputFrame
函数功能 : 送入一个不含起始码的 H.264 NALU。函数把数据拷进队列，返回后调用方可以复用缓冲。
              队列满时丢掉最旧的一帧。该函数不阻塞。
输入参数说明 : nal 指向 NALU，含一个字节的 NAL 头。pts90k 是 90 kHz 时间戳。
函数返回值的说明 : 成功返回拷入的字节数，失败返回 -1。
*************************************************/
int RTSPServer_InputFrame(int hServer, const char *name,
                          const unsigned char *nal, int len, unsigned int pts90k);

/*************************************************
函数名称 : RTSPServer_RemoveStream
函数功能 : 卸下一路流。已连接的会话不再收到后续帧。
函数返回值的说明 : 成功返回 0，失败返回 -1。
*************************************************/
int RTSPServer_RemoveStream(int hServer, const char *name);

/*************************************************
函数名称 : RTSPServer_Start
函数功能 : 开始监听，并启动已经挂上的文件线程。
函数返回值的说明 : 成功返回 0，失败返回 -1。
*************************************************/
int RTSPServer_Start(int hServer);

/*************************************************
函数名称 : RTSPServer_Stop
函数功能 : 停止监听和全部会话。句柄仍可用，Destroy 再释放。
函数返回值的说明 : 成功返回 0，失败返回 -1。
*************************************************/
int RTSPServer_Stop(int hServer);

#ifdef __cplusplus
}
#endif

#endif
