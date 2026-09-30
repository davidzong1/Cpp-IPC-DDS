# socket 千路的线程成分归因（t33）
# mode1 = 只建 300 个**订阅者**（socket_sub_ipc）
# mode2 = 只建 300 个**发布端**（socket_pub_ipc）

mode1: mode=1 n=300 pool_workers=32 pool_routes=299 TOTAL_threads=33
mode2: mode=2 n=300 pool_workers=0 pool_routes=0 TOTAL_threads=301

⇒ 额外线程**全部来自发布端**（socket_pub_ipc 的 discovery_loop，每实例 1 条）；
   订阅侧经 SocketRecvWorkerPool 固定 32 worker，**无 per-route 接收线程**。
   证据：mode1 total=33 = 32 worker + 1 主线程；mode2 total=301 = 300 发布端 + 1 主线程。
