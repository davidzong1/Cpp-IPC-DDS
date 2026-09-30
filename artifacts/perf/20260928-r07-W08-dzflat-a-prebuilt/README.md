# 20260928-r07-W08-dzflat-a-prebuilt

transport=dzflat-a variant=prebuilt process_model=cross-process work_package=W08

生产端 = fork 出来的子进程(shm_pub_ipc), 消费端 = 父进程(shm_sub_ipc)。发一条取一条(子进程等 ack), 以规避 docs/dzflat_shm.md §9.5 末尾的 chunk 池耗尽既存缺陷。

本 run 的样本数=120 完整载荷校验通过=120 丢失=0

生产端路径计数(生产进程自报): cfg=dzflat-a variant=prebuilt expected=120 seq_ok=120 seq_fail=0 dzflat_a_messages=120 dzflat_b_messages=0 tlv_messages=0 dzflat_publish=120 dzflat_fallback=0 prebuilt=0 dzflat_wire_bytes=50022720 tlv_wire_bytes=0

