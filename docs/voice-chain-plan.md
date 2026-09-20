# 语音链路：唤醒 → ASR → LLM → TTS（设计备忘，2026-09-20）

状态：设计。★ = 实测日志里有上板/主机实测条目；其余为读码结论或设计。

## 现状

- 三棵树从未合并：`tmp/amp-compute-draft-20260914`（ASR/TTS 协议帧 43 个主机用例通过、
  共享内存双端驱动★、三个模型后端 `nyamp_sherpa/melo/rkllm.cpp`）、`tmp/product-wt`
  （产品固件；协议头只有 health、无 shmem）、`tmp/melo-npu-20260912` 等实验目录。
  **第一件事是把草稿分支并进 product 分支**，不是写新代码。
- ASR：sherpa-onnx 1.13.8 流式 Zipformer-zh-14M INT8（约 24 MiB），16 kHz 单声道
  float32，A72 单线程 RTF≈0.127★（三模型并发时 0.96）。后端现为"整段喂完再解码"。
  `nyampd_asr.cpp` 不存在；协议 `ASR_LOAD/BEGIN/PUSH/RELEASE/CANCEL` +
  `EVENT_PARTIAL(增量文本)/FINISH` 已定义。音频走共享内存 `SLOT_SHARED`（1 MiB，
  与 TTS 输出交替使用），窗口 1 s=64000 B，租约由 Linux 单边铸造。
- TTS：MeloTTS zh_en，CPU 前半（prefix.onnx 111 MB，现只有本机三段分片
  `PFX00/01/02.BIN` + `PREFIX-PACK.json`）+ NPU 声码器 `MASK512.RKN`（39 MB）。
  真实中文句 3.97 s 音频耗时 2.55–2.60 s（RTF≈0.65）★，首次加载 5.7 s。输出 44.1 kHz
  单声道 float32；**固定 512 帧桶 ≈5.94 s 上限，长句必须切分，禁止截断**；
  `valid_samples = 帧数×512`（342 帧应为 175104）。**没有 G2P**：协议 `TTS_SYNTH` 收的是
  音素/声调 id；词典资产在 `tmp/media-amp-20260912/melo-inspect/vits-melo-tts-zh_en/`
  （lexicon.txt 6.8 MB、tokens.txt、dict/、*.fst）。`nyampd_tts.cpp` 不存在。
- 唤醒：无模型无代码。路线 A（零新模型）= 对流式 ASR 的 partial 文本做模糊匹配
  "你好 openvela"（zh 模型会把 openvela 转成近音汉字，匹配要按拼音/模糊尾部，命中率
  待 20 句实测）；路线 B = sherpa KWS zipformer（约 3 MB，支持自定义关键词，未下载）。
- 音频★：`pcm_in0` 16 kHz 单声道 S16 录音可用（需显式指定设备），`pcm0` 44.1 kHz 可放，
  所以两端都免重采样，只需 S16↔float32 与单/双声道适配。AMP 下 product 固件
  `audio ready` 已于 2026-09-20 实测。
- Agent：语音回合应新增非 static 包装（如 `ny_agent_voice_submit(conversation, text)`）
  调 `ny_agent_submit()`（ny_agent.c:930，需 requestId/conversationId/text，单回合互斥，
  忙时 -EBUSY）；回复经 `message_bus_pop_outbound` 一次性给出整段文本（无 token 流）。
  取消：`agent.cancel` 已有，跨 OS 的 CANCEL 只有标志位没有流程。
- 眼睛：listening→`curious`，thinking→`processing`，speaking→`happy`（或 `caption` 场景
  做字幕）。`eyes.expression` 是 1 s 同步确认，**不得在音频线程里调**。

## 设计

NuttX 线程：`ny_voice_capture`（独占 pcm_in0，S16→f32，RMS 门限 VAD，写 4 s 预录环）、
`ny_voice_pump`（环→shmem 窗口→ASR PUSH）、`ny_voice_sm`（状态机、唤醒匹配、提交
agent、驱动眼睛）、`ny_voice_play`（消费 EVENT_PCM，f32→S16，写 pcm0）。
Linux：每个服务照抄 `nyampd_llm.{h,cpp}`（单 Session + 工作线程 + 有界事件队列 + 事件泵）。

状态机：IDLE —有声→ SENSING —partial 命中唤醒→ LISTENING —静音 1.2 s→ THINKING
→ SPEAKING（期间麦克风静音防自唤醒；VAD 打断 = TTS_CANCEL + 清 pcm0）→ IDLE；
LISTENING 最长 8 s。预录环必不可少（命中时用户已在说指令）。

协议：ASR/TTS 不需要新 opcode；G2P 放 Linux 侧时新增 `TTS_SYNTH_TEXT`（UTF-8 文本），
同时改掉头文件里"不收文本"的注释；KWS 若走路线 B 用 service id 9 以后的空号
（注意 BLOB 服务也要占号，统一分配）。

## 顺序（每步可独立验收）

1. 并入草稿分支；`test_shmem_layout.py` 与板上 `nyampctl shmem` 复现 `1048572 words match`。
2. 复核 AMP product 固件下音频设备在。
3. `nyampd_asr.cpp`，主机侧用 `test_wavs/0.wav` 对齐 `nyamp_asr_file_test` 的转写。
4. NuttX 推流：先回放已知 PCM 文件过 shmem 与第 3 步结果对齐，再换真麦克风。
5. VAD + 路线 A 唤醒 + 预录环；20 句命中率、10 分钟环境误触发（目标 <1 次）。不行就上路线 B。
6. G2P + 分句（纯主机）：金标准是已知句 `你好，我是星喵。…` 必须产出与 `X.BIN/TONES.BIN`
   完全一致的 95 个 id。
7. `nyampd_tts.cpp` + NuttX 播放：先验 `valid_samples==175104`，PCM 先落 WAV 用 nxplayer 听，
   再信自己的播放线程。
8. 状态机 + 眼睛 + 打断；演示句"你好，openvela，现在几点"。
首音延迟：LLM 整段生成 + TTS，估 4–7 s；先做按句流水 TTS 与本地"我在"应答音掩盖。
