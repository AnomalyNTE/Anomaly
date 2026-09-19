# mmd2bip → 插件内置转换器：移植规格书

状态：**已确认路线，待一份骨架数据（头环判定）**。作者：本会话（2026-09-14）。
已定：C++ 移植进插件、零依赖；发丝自动「只弯不拧」；头环自动刚性；参考模型只随包带骨骼表；
手指 roll 暂不改（保持 `hand`）。
目标读者：实现该移植的 agent/人。实现时**以本规格 + `mmd2bip.py` 为准**，两者冲突时以
`mmd2bip.py` 的当前行为为准（它是唯一已被游戏内验收过的实现）。

## 0. 目标与非目标

**目标**：BetterPose 插件里「选一个 VMD → 自动用**当前角色**的骨架导出一份可播放的
`better-pose-motion` JSON」，零外部依赖（不要求玩家装 Python），不暴露任何转换参数。

**非目标**：
- 不改转换的数学（本规格是**行为等价移植**，不是重新设计）。
- 不做 PMX/VMD 的导入 UI；参考模型的骨架数据随包携带。
- 不在此规格内重做 `--finger-roll` 的逐角色策略（见 §6 待确认）。

## 1. 输入 / 输出（四个输入，一个输出）

| 角色 | 现在（命令行） | 内置后 |
| --- | --- | --- |
| 动作 | `--vmd <motion.vmd>` | **用户选择**（唯一必选） |
| 动作关键帧 | `--vmd-json <dump.json>`（`vmd_dump.py` 产出） | 插件内 VMD 二进制解析 |
| 目标骨架 | `--skeleton <pose.skeleton.json>`（游戏内 Export Skeleton） | 插件自己导出当前角色（同一份 `baseLocal/refLocal` 结构） |
| 参考模型 MMD 骨架 | `--pmx <pmx.json>`（`pmx_dump.py` 产出；当前固定为「学马仕 藤田琴音 常服」495 骨） | 随包数据文件（见 §8） |
| 输出 | `--out <motion.json>` | 自动命名 + 落到插件数据目录，供 `load` |

**参考模型是固定的**：它只提供 MMD 侧的骨骼位置/尾巴方向（转 axis 后作为方向参考），
与角色无关；换角色不改它。

### 1.1 输入格式规格

**`--skeleton`（插件导出，已有）**
```
{ "schemaVersion":1, "kind":..., "basis":..., "mesh":"0x3FF786670",
  "boneCount":253, "bones":[ {"index":0,"name":"Root","parent":-1,
    "baseLocal":{"rotation":[x,y,z,w],"translation":[x,y,z],"scale":[1,1,1]},
    "refLocal":{...}, "liveComponent":{...} }, ... ] }
```
- `bones` 必须**父在子前**（`parent < index`），否则 `load_skeleton` 直接报错
  （`mmd2bip.py:361-367`）。插件导出侧已保证。
- `refLocal` 齐全时才用「引擎参考姿态」（`use_ref`，`mmd2bip.py:675-681`），否则用捕获姿态。

**`--pmx`（`pmx_dump.py` 产出，随包）**：`bones[]` 需要 `name / index / parent / position /
tailBone? / tailOffset?`。`bytesConsumed == fileSize` 是解析完整的证明。

**`--vmd-json`（插件内解析，等价 `vmd_dump.py`）**：只需
```
{ "modelName":str, "boneFrameRange":[lo,hi],
  "bones": { "<MMD骨名>": [ {"frame":int,"pos":[3],"quat":[x,y,z,w]} , ... ] } }
```
VMD 二进制布局（`vmd_dump.py:52-113`）：header 30 字节（`Vocaloid Motion Data file`／含 `0002`
则 v2）→ 模型名（v1 10 字节 / v2 20 字节, Shift-JIS）→ `u32` 骨关键帧数 → 每帧
`{name(15, Shift-JIS), u32 frame, f32×3 pos, f32×4 quat(x,y,z,w), 64B 插值}`
→ 之后 morph / 相机 / 灯光 / 阴影 / 可见帧 / IK 段也必须走完（用于 `walkExact` 自检）。
**插值字节当前被忽略**（`interp` 只记录 hex），采样用端点保持 + slerp，见 §3.4。

## 2. CLI → 内置映射

内置后**没有任何开关**；下表说明每个命令行参数的归宿。右列「自动规则」= 用户已确认的
内置判定（§5）。

| 参数 | 默认 | 内置后 | 依据 |
| --- | --- | --- | --- |
| `--fps` | 30.0 | 从 VMD 关键帧推（当前素材 30） | `mmd2bip.py:574,975-983` |
| `--max-frames` | 0（全段） | 全段 | — |
| `--sim-secondary` | 1 | 开 | §3.7 |
| `--sim-stiffness` | 200.0 | 200.0 | 实测稳定 |
| `--sim-damping` | 16.0 | 16.0 | 实测稳定 |
| `--sim-gravity` | 0.0 | 0.0（下坠方向由 `sim-drape` 给） | — |
| `--sim-max-lag` | 25.0 | 25.0 度 | 硬钳制 |
| `--sim-drape` | 0.15 | 0.15 | — |
| `--sim-inertia` | 0.5 | 0.5 | — |
| `--root-vertical` | 1.0 | 1.0 | 当前素材与所有构建脚本都是 1.0 |
| `--twist` | 1 | 1 | 扭转分摊 |
| `--track-untracked` | 1 | **恒为 1**（不可关） | 「固定≠不给轨道」 |
| `--rest-source` | bind | bind（用 `refLocal`） | — |
| `--hand-rest` | live | live（自动在两候选中取更小修正量） | `mmd2bip.py:731-776` |
| `--hand-plane` | 0 | 0（**不启用**：测量脚本自相矛盾） | `mmd2bip.py:604-608` |
| `--finger-mapping` | aligned | aligned | — |
| `--finger-twist` | 0.0 | 0.0 | — |
| `--no-direction` | 空 | 空（方向修正走自动规则） | — |
| `--finger-roll` | hand | **自动规则（默认 own）** | §5.3 待确认 |
| `--no-sim` | 空 | **自动规则**（头环类） | §5.2 |
| `--sim-scale` | 空 | 空（保留实现，规则暂不用） | — |
| `--sim-swing-only` | 空 | **自动规则**（发丝类） | §5.1 |

## 3. 转换器行为清单（移植单元）

每个小节是一条必须在 C++ 里逐位复现的规则；括号内是 `mmd2bip.py` 的行号。

### 3.1 四元数基础（`39-108`）
`qmul / qconj / qrot / qnorm / qslerp / qswing / unit`；约定 `(x,y,z,w)`，
`qrot(q,v)` 用 `t = 2·(q_v × v)` 的展开式。数值细节必须一致：
- `qslerp`：点积为负先翻号；`d > 0.9995` 走线性插值（**阈值不能改**）。
- `qswing(u,v)`：`d > 1-1e-12` 返回单位；`d < -1+1e-12` 取垂直轴、w=0；否则 `normalize(cross, 1+d)`。
- `A_AXIS = (0.7071067811865476, 0, 0, 0.7071067811865476)`（MMD→Bip001，绕 X +90°）。

### 3.2 骨骼映射表（`111-254`）
- 静态表 `MAPPING`（22 条，含 `sources`（MMD 侧链式相乘）与 `dir`、`position` 标志）。
  「对应 MMD 骨名全部存在」才产生映射。
- 手指链：MMD 全角数字（`左人指１`）与目标数字做**归一化比较**（`203-208`）。
  目标每指 4 节（`Finger1/11/12/13`），MMD 3 节（`人指１/２/３`）：
  按**累计长度分数**贪心最近对齐（`align_chains`，`169-183`），未被对齐的领先骨输出
  **静止轨道**（`static`，`251-253`）。
- `FINGER_PREFIX` / `FINGER_JOINTS`（`142-144`）。

### 3.3 骨骼方向与 rest 姿态（`379-452`, `784-864`）
- `bone_dir_local`：叶子骨用「延续入射段」（自身 `baseLocal.translation` 经
  `qconj(自身 rotation)`），非叶子骨**只在 `Bip001*` 子骨里挑最长偏移**；没有解剖子骨时
  同样退回延续段。**这两个规则是硬结论**（头部 37° 倾斜、躯干被背包带拽歪都是踩过的坑）。
- `chain_direction`：优先用「MMD 侧首个子骨映射到的目标骨」做方向（`438-452`）；
  否则退回 `bone_dir_local`。
- 起点：`use_ref` 时 `baseLocal := refLocal`（`675-681`）。
- 手链特例：`--hand-rest live` 时**测两种候选的最坏修正量**，取更小且 ≤90° 的那个，
  把 `-Hand` / `Finger*` 的 rest rotation 换回捕获姿态（translation 仍用 refLocal），
  之后重建 `crot_cap` 与 `cpos`（`731-782`）。
- 方向修正：对每个有映射的骨，`rest_crot[i] = qswing(have, want) · base`，其中
  `want = qrot(A_AXIS, mmd_dir)`、`have = qrot(base, u)`；**修正角 > 90° 则放弃该骨**
  （记录 `dir_bogus`，保留参考姿态）（`818-832`）。
- **豁免**（不做方向修正，只把增量作为 delta 叠上）：`Finger*`、`*-Hand`、`Foot`、`Toe`，
  以及 `--no-direction` 命中项（`809-814`）。理由：PMX 手指尾巴全部朝 +X（四条方向相同）、
  脚/趾方向是标记而非方向。
- 手指 rest 的 roll 来源（`--finger-roll`）：`own`＝自己的捕获帧；`hand`＝所属手的
  `rest_crot`；`world`＝`A_AXIS`（`834-849`）。
- 未驱动/无方向参考的骨：保留捕获 local rotation，但 component 旋转必须用**已修正的父**
  重建（`857-864`）。

### 3.4 采样（`258-293`）
`build_sampler`：按帧号 `bisect`，端点保持，键间 slerp（`qslerp(qnorm, qnorm)`）+
位置线性插值。**命中不到关键帧时绝不能返回单位四元数**——这是抽动的根因。

### 3.5 帧与输出骨架（`975-983`, `990`, `1140-1164`）
- 输出帧 = 关键帧范围 `[frame_lo, frame_hi]`，步长 `round(native_fps / fps)`（当前 1:1）。
- 每帧每骨：`delta = A_AXIS · Q · conj(A_AXIS)`（世界增量），
  `local = conj(Cp(rest)) · delta · Cg(rest)`，`qnorm`，写 6 位小数。
- 根位移：输出 **MMD 单位的增量**（相对首帧），并已转到目标轴（`qrot(A_AXIS, delta)`），
  `--root-vertical` 只缩放 Z（`1165-1186`）。运行时按角色腿长缩放。

### 3.6 单位换算（`1102-1118`）
`scale = 目标腿段长 / MMD 腿段长`（`左足→左ひざ→左足首` 两段之和 vs
`Bip001-L-Thigh/Calf/Foot` 的 `baseLocal.translation` 模长之和）。写进
`unitScaleCmPerMmdUnit`。

### 3.7 次级运动（发丝/布料/裙摆）弹簧（`993-1339`）
- 判定 `is_secondary`（`1000-1008`）：排除名含 `twist/finger/ik`；`Bn_` 前缀直接算；
  否则名字含 `hair/qun/cloth/tail/piao/gongpai/lalian/xiong/tie/skirt/ribbon`。
- 只对**未被映射**的骨模拟；`rigid` 集合（头环等）跳过。
- 状态：`dir`（世界方向）、`w`（角速度）；每帧 `substeps = 4`，`h = dt/4`，
  `w += (stiffness·(d×target) − damping·w)·h`，再按 `d += w×d·h` 积分并归一化。
- 目标方向：默认 rigid 方向；`sim_drape > 0` 时与「重力(0,0,-981) 减父加速度×inertia」
  混合（加速度钳 3 g，`1229-1250`）。
- 钳制：`lag > sim_max_lag` 时按比例拉回并把 `w *= 0.5`（`1275-1283`）。
- 输出 local：`qswing(u, 父系下的 d) · twist_about(s.local, u)`。
- `swing_only`：把偏差四元数里的「绕自身轴的 roll」除掉（公式 `1297-1299`）。
- `sim_scale`：`scale_quat_angle(deviation, gain)`（保轴缩角，`302-318`）。

### 3.8 扭转骨（`321-357`, `1132-1137`, `1312-1340`）
`(Bone|Bip001)-(L|R)-(UpperArm|ForeArm|Thigh|Calf)-Twist(1?)`：
`weight = 0.5`（Twist）/ `0.25`（Twist1）；角度取 delta 在该骨轴上的投影
（`2·atan2(v·u, w)`），乘 weight 后**硬钳 ±0.35 rad**，绕轴施加。

### 3.9 每骨有轨道（`1342-1360`）
未被驱动、未被映射的骨（`track_untracked=1` 恒开）输出**恒定轨道** = 参考姿态的 local
rotation（6 位小数），使其刚性跟随父骨。**这是修「钉在场景里」的关键**。

### 3.10 文档头（`1366-1403`）
必须写全：`schemaVersion/kind("better-pose-motion")/source/targetMesh(=骨架的 mesh)/
rootBone/fps/nativeFps/firstFrame/frameCount/unitScaleCmPerMmdUnit/mmdLegLength/
rootTranslationUnit("mmd")/axisChange/rest/bones/rootTranslation/diagnostics`。
运行时读 `fps/targetMesh/frameCount/rootBone/rootTranslationUnit/mmdLegLength`
（`plugins/BetterPose/plugin.cpp:2550-2604`）。

### 3.11 源模型自身的 IK（`mmd2bip.py` `MmdIkPose`，2026-09-14 新增）

**为什么必须有**：VMD 对腿只写 IK 骨的**位置**。`主角.vmd` 里 `左足ＩＫ` 36 帧有 30 帧带位移，
而 `左足`/`左ひざ`/`左つま先` 的 9 个关键帧**全是单位旋转**——没有解算器，腿只能停在绑定姿势
（就是「腿完全不跟随」）。参照模型有 4 条链：`左/右足ＩＫ`（target `左/右足首`、loop 40、
每步限角 2.0 rad、links `[ひざ(限 [-π,0,0]..[0,0,0]), 足]`）＋ `左/右つま先ＩＫ`（target
`足首_L_/R_`、loop 3、限角 4.0、links `[足首]`）。

**顺序**：每帧先给 MMD 骨架做完整 FK（含骨骼位移），再按骨序解 IK，然后把解算后的**局部旋转**
当作源旋转送进映射管线。骨骼变换用 mmd_tools 约定：位移在自己的 rest 坐标系里，
`world = parent_world · T(rest_offset) · T(vmd_position) · R(vmd_rotation)`，
`rest_offset = position − parent.position`（MMD Y-up）。

**迭代**：每轮从末端侧的 link 往根部走，取 `(末端当前位 − link)` 与 `(IK 骨 − link)` 的夹角，
按 `min(angle, limitRadian)` 绕叉积轴旋转；带限角的 link 再把得到的**局部**旋转转成
`Rx·Ry·Rz` 欧拉、按 `limitMin/limitMax` 逐轴钳制后转回（膝盖的 `[-π,0,0]..[0,0,0]` 就是
「只能往一个方向弯」的那一个自由度）。某轮所有 link 都不动即提前结束。

**这不是 CC 死锁**：CCD 对「髋-膝-踝严格共线」的目标无解（旋转改变不了距离），但真实模型的膝
本来就偏出髋踝连线 0.116 单位，而且**本模型的 IK 骨放在腿长之外**（髋到 IK 骨 9.3732，
腿可达 9.2085 ⇒ slack −0.1648），所以休息姿势本身就残留 ~0.16 单位——那是模型，不是解算器。
`--ik 0` 完全跳过解算，输出与加 IK 之前逐位相同（回归开关）。
量化验收：可达目标残余 **1e-5~0**；不可达目标残余 **= 髋到目标距离 − 腿可达长度**（实测
0.1708 vs 0.1648、41.4643 vs 41.4613）；膝盖两侧一致度 0.9997；限角全部满足
（`tools/mmd2bip/ik_solver_check.py`，断言的是解析事实而不是与实现共享的假设）。

**IK 开关轨**：VMD 尾部**只有一段**（不是「可见性段 + IK 段」两段）：
`visibleKeyCount(u32)`，每帧 `frame(u32) + visible(u8) + ikCount(u32) + ikCount×[name(20, Shift-JIS) + on(u8)]`
（`vmd_dump.py:101-112`）。**7 个样例文件用这套布局全部 walk 精确**——之前报 MISMATCH 是我把
它当两段读。语义＝MMD 的 IK 默认开，记录是**状态变更**（文件里顺序乱，按帧排序后取
`frame ≤ f` 的最后一条）；本模型这条链的名字也是 Shift-JIS，用原始字节读永远匹配不上 UTF-8
骨名（会「读了但一条都没禁用」）。已用「把 `主角.vmd` 里 `左足ＩＫ` 的 16 条记录全翻成 0」验证：
两边都只解 3 条链、输出互相 355/355 一致，且确实改变了输出（`Bip001-L-Foot` 1.086 分量、
`L-Calf` 0.538）。

**IK 骨自身的旋转要用**（别再以为用不到）：`左足ＩＫ` 的子骨就是 `左つま先ＩＫ`，转它会带动
脚尖链的目标位置。实测 `fktn-001` 与 `Motion` 的 `左/右足ＩＫ` **每一帧**都带非单位旋转
（2368/2368、2482/2482）；把它们的旋转归零后 `Bip001-L/R-Foot` 差到 0.985/0.802 分量、
脚趾配件骨最多 48.2°（`主角` 分别是 0.295、14.9°）。实现里它们照常参与 FK 即可。

### 3.12 根运动＝整条根链，不是 センター 一根骨（2026-09-14 修正）

**症状**：`Chu-Chu-U-Chu公开部分.vmd` 看起来「有根运动但走不动，像太空步、绕着中心点转」。

**根因**：映射是 `Bip001 ← センター`、`Bip001-Pelvis ← 腰`，而**目标骨架的 Bip001→Pelvis 是固定
局部偏移**——所以参照模型里 `全ての親 → センター → グルーブ → 腰 →（下半身）` 这一整条链的位移都必须
并进根运动。旧实现只取 **センター 自己的位移**，把下面各层全丢了。

| 文件 | 旧（センター 自身） | 身体实际（根链） | 差 |
| --- | --- | --- | --- |
| fktn-001 / Motion | — | — | **0.000**（只有 センター 在动，公式等价） |
| Chu-Chu-U-Chu | y 0.000 | **y 6.238**（≈60 cm 起伏） | max 3.95 |
| 主角 | x 4.078 z 3.597 | x 13.676 z **30.179** | max **27.6** |

而腿的旋转是 IK 解算出来的、**看得到整条链**（解算时的 FK 含所有位移）⇒ 腿按「身体抬起来了」去弯、
身体却没升 ⇒ 脚在地面上滑、看着像太空步。修好之后两者才一致。

**公式**（`chain_translation`）：只累加**平移**部分，`Σ Rot(wrot[parent_i], pos_i)`。
旋转**不能**再算一遍——链上方的旋转已经由「映射到 Bip001 的旋转」承担了，加进去就是重复计入；
这也正好让只有 センター 在动的文件（fktn/Motion）得到与旧公式**完全相同**的结果。
链的末端＝**链上最深的一根真有位移的骨**：`TDA_30fps` 的位移在 `下半身`，停在 センター 会整段丢掉
（诊断会打成 `root 下半身(data)`）。

独立验证：`compare_motion.py` 的 `rootTranslation` 逐值比对（fktn 关/开 IK 都是 0.000e+00，
Chu-Chu 两边 355/355 且根运动完全一致）；`root_formula.py` 直接对拍新旧公式。

## 4. 不可改动的常量与阈值（都来自实测）

| 常量 | 值 | 为什么不能动 |
| --- | --- | --- |
| 方向修正放弃阈值 | 90° | 趾骨 119.7° 是标记噪声；手指 50°/手 65°/骨盆 18.5° 合法，阈值必须留出余量 |
| Twist 钳制 | ±0.35 rad | 无钳制实测 164°（前臂会转飞） |
| 弹簧子步 | 4 | 单步 30 fps + 刚度 200 只在稳定边缘 |
| 加速度钳制 | 3 g | 中心轨稀疏，二阶差分尖峰会让悬挂方向乱甩 |
| `qslerp` 线性阈值 | 0.9995 | 改变会影响抽动修复后的数值 |
| `qswing` 反平行阈值 | 1e-12 | 同上 |
| `kExtraMeshMatchCm=2.0` / `kExtraMeshBindCm=1.0` | 插件侧 | 本次清理未动；与转换器无关，列出避免误改 |

## 5. 自动参数规则（三条全部收口：发丝通用、头饰骨名表、手指 roll 不可达）

### 5.1 发丝：只弯不拧 ✅ 已确认（**小吱前发实测：拧占 100%**）
所有在 §3.7 里被判为次级、且名字命中 `hair` 的骨，自动进入 `swing_only`。理由不是角色特异：
这套 rig 的发丝弹簧弯 4–7°、拧 27–44°，扁发片一拧就「原地打转」并刮过头穿模。

**小吱（mesh `0x19ED057780`）实测**——把每根发骨相对自身 rest 的偏差拆成「绕自身轴拧」与
「摆动」两部分（`.local/vmd2anim-ref/twist_check.py`）：

| 骨 | 默认：偏差 max/mean | 其中拧 max/mean | 拧占比 | 加 swing-only 后 |
| --- | --- | --- | --- | --- |
| `Bn_m_hairF_001`（前发） | 179.7° / 56.7° | 179.7° / 56.4° | **100%** | 偏差 17.7° / 4.3°，拧 1.4° / 0.3° |
| `Bn_l_hairA_001` | 60.8° / 13.7° | 59.4° / 12.1° | 88% | 17.1° / 5.3°，拧 5.7° / 1.1° |
| `Bn_l_hairBa_001` | 100.3° / 20.0° | 99.3° / 19.1° | 96% | 17.6° / 4.7°，拧 5.0° / 0.8° |
| `Bn_m_hairT_001` | 23.2° / 6.2° | 2.5° / 0.5° | 8% | 23.1° / 6.2°（本来就只摆） |

⇒ **前发那根 180° 的「转」就是弹簧的 roll**，`swing-only` 直接把它从 179.7° 压到 1.4°。
注意副作用：A/B 组发丝的总偏差也从 60–100° 降到 ~17°（拧被拿掉后摆动目标也跟着小了），
所以视觉上会**更收敛**；`--sim-scale` 只能缩小不能放大（gain 被钳到 1.0），要更大摆幅得调
弹簧参数（stiffness/damping）而不是这个开关。

验证产物：`D:\pose-motion-xiaozhi-autohair.json`（headProp+hat 刚性 + **自动**发丝
swing-only；与显式 `--sim-swing-only hair` 的版本**逐帧完全一致，0 条轨道差异**）。

**已实现且通用（2026-09-14，`mmd2bip.py`）**：`SWING_HAIR_WORDS = ("hair", "hairline",
"hari", "kami", "髪", "かみ")` 作为子串命中 bone 名即算发丝，自动进入 swing-only，
**不依赖命令行、不依赖角色**；`--no-swing-hair` 是唯一的 A/B 逃生开关（实测它精确回退
那 89 条发丝轨道）。判定与角色无关的验证：

| 数据 | 总 `Bn_` | 判为发丝 | 判为头饰（hat/prop/…） | 其余（保持默认拧） |
| --- | --- | --- | --- | --- |
| 小吱骨架 379 骨 | 282 | 136 | 9（`headProp`+hat 链） | 137（ribbon/rabbit/bow/belt/qun…） |
| 伊洛伊已导出动作 379 轨 | 268 | 116 | 24（`hatAll`/`hatSpineA-F`） | 128（`qun`/`tie`/`xiu`/`string`…） |

⇒ 命名词命中即可，**不需要逐角色配置**；新角色若把头发叫别的名字，再用
`--sim-swing-only <词>` 补，并在移植时把该词加进 `SWING_HAIR_WORDS`。


### 5.2 头环/头饰：自动刚性 ✅ 已收口（三个角色全部进内置骨名表）

**先回答「自动加动骨到底判断什么」**（`mmd2bip.py:1000-1008` 的 `is_secondary`）：

1. 名字含 `twist` / `finger` / `ik` → **不模拟**；
2. 名字以 `Bn_` 开头 → **直接算次级骨（会模拟）**——`Bn_` 这一条**优先于描述词**，
   所以「头上没有 hair 的名字也会动」；
3. 其余名字含 `hair/qun/cloth/tail/piao/gongpai/lalian/xiong/tie/skirt/ribbon` → 模拟。

次级骨由弹簧驱动：**跟父骨旋转之间的滞后，`--sim-max-lag` 25° 上限**（实测小吱
`lag mean 4.3° / p95 13.6° / max 25°`），所以头上任何被判为次级的几何都会跟着摆。

**实测（小吱，mesh `0x19ED057780`，379 骨，`headprop`；2026-09-14 本会话）**：

| 头骨下的链 | 骨数 | 映射 | VMD 轨道 | 现状 | 判定 |
| --- | --- | --- | --- | --- | --- |
| `Bn_l/r_hair*`（33 条根里的 31 条） | 3–16 | 0 | 0 | **摆动**（期望） | 保持模拟 |
| `Bn_m_hat_001` → `Bn_l/r_hatFlyA/B_001` | 8 | 0 | 0 | **摆动**（「没有头发名也会动」就是它） | 应刚性 |
| `Bn_m_headProp_001`（她说的环） | 1 | 0 | 0 | **恒定轨道，不动** | 单骨叶子**没有子偏移 ⇒ 弹簧没有轴**，代码天然跳过；离头 24.8 cm |

结论：**单骨链（叶子）本来就不会被弹簧带**，所以「环会动」通常不是环骨自己在动，而是它周围
被判为次级的链（小吱这里是 `Bn_m_hat_001` 那 8 根）在摆，视觉上把环一起带走。规则要能覆盖
这两种情形，且不能误伤发丝。

**候选规则**：头骨直接子链中，**全部骨都没有 MMD 映射、没有 VMD 轨道**，且链根名字含
`hat / prop / line / ring / horn / glass / crown / halo` ⇒ 整链刚性（等价 `--no-sim`）。
已验证产出 `D:\pose-motion-xiaozhi-headfixed.json`
（`--no-sim Bn_m_headProp_001,Bn_m_hat_001`）：8 根 hat 骨进 `rigidBones`、hat 轨道恒定、
发丝仍动，其余轨道与默认版逐帧一致；用户在游戏内确认「帽子/环不摆了」。

**头环/头饰的最终做法（用户 2026-09-14 定）**：**这类挂件一共只有三个角色，直接按骨名放进
表里匹配**，不做启发式判定。已实现：`mmd2bip.py` 的 `HEAD_ORNAMENT_BONES`（精确骨名 +
整棵子树，默认生效），配套 `--no-sim-names <name,...>`（临时加）与 `--no-sim-builtin 0`
（关掉内置表）。表内现有：

| 角色 | 骨名 | 形状 |
| --- | --- | --- |
| 小吱（mesh `0x19ED057780`） | `Bn_m_headProp_001` | 单骨（无子偏移 ⇒ 本来就不会被弹簧带） |
| 浔（mesh `0x181ABE5560`，527 骨） | `Bn_m_headProAa_001`、`Bn_m_headProAb_001`、`Bn_m_headProAc_001`、`Bn_m_headProB_001` | 四件（`Aa` 有 `Ab/Ac` 两个子骨，`B` 是单骨；四者绑定偏移相同 `[16.9,15.9,0.3]`，离头心 23.2 cm） |

第三个角色（伊洛伊的 `headLine`）等下次导出拿精确骨名补进来。

实测（浔，`D:\pose-motion-xun.json`，527 轨 / 2368 帧）：四件 `headPro` 轨道**恒定**
（`distinct = 1`），78 条发丝轨道仍在动。**注意**：在默认参数下这四件本来就是 0.0° ——
`Bn_m_headProAa_001` 有子骨、可被弹簧拖，但父骨是头骨、`lag` 很小；所以这条表在这个角色上
是把结果**写明确**，真正会大幅摆的是同一批 `Bn_*_headPiao*` / `Bn_m_headFly*` / `Bn_*fly*`
（飘带/小飞饰，实测 24–155°），那些**没有**进表。

注意：**单骨挂件本来就一动不动**（没有子偏移 ⇒ 弹簧没有轴 ⇒ 跳过），表对「挂件是单骨」的
角色只是把结果写明确；真正需要它的是**挂件是一条链**的角色（父骨有偏移才会被弹簧拖）。

**已证伪**：「头骨下 unmapped + 无 VMD 轨道 ⇒ 刚性」会命中娜娜莉的 11 条、小吱的 31 条
发链，把头发全冻住（判据脚本 `.local/vmd2anim-ref/head_chains.py`）。


### 5.3 手：现在实际生效的是哪几个参数（2026-09-14 实测）

| 参数 | 现值 | 作用 |
| --- | --- | --- |
| `--hand-rest` | `live`（自动选） | **真正在管事**：手骨+手指的 rest 来源，两个候选各算最大修正量取更小且 ≤90° 者。小吱实测「捕获 126° vs 参考 70°」⇒ 选了**参考姿态**（`hand chain rest: reference pose`） |
| `--finger-mapping` | `aligned` | 4 节目标链按**累计长度**对齐 MMD 3 节链，指尖（`Finger13`）也有轨道；多出的第一节输出静止轨道 |
| `--finger-twist` | `0` | 未用 |
| `--hand-plane` | `0` | 未启用（当时的测量脚本自相矛盾） |
| `--finger-roll` | `hand`（代码默认） | **当前不影响输出**：手/手指/脚/趾被排除在方向修正之外（豁免分支先 `continue`），roll 那段代码**不可达**。实测 `own/hand/world` 三份输出的骨头负载 **sha1 完全相同**。README/help 里「游戏内 A/B 选了 own」是**过期说明**，已改正 |

⇒ 移植时手部只需照搬 `--hand-rest` 的自动选择 + `aligned` 链对齐；`--finger-roll` 可以
原样保留成「预留但不可达」，**不要**据此设计逐角色配置。



## 6. 移植阶段与验收（不在游戏里对数值）

**进度（2026-09-14）**：阶段一**已通过验收** —— 60/60 映射骨在 2368 帧上容差 ≤1e-6 一致，
`rest` 映射与 `rootTranslation` **逐值相同**，355 条轨道集合一致，文档头字段一致；剩余 151 条
差异全部是未映射的配件骨（弹簧次运动，属阶段二）。实现见
`plugins/BetterPose/retarget/motion_builder.{hpp,cpp}`，验收工具 `apps/mmd2bip/main.cpp`
（构建目标 `anomaly_mmd2bip`）。

**阶段一移植中发现的四个坑（都已修，写在这里免得复现）**：

1. **VMD 骨名是 Shift-JIS，映射表是 UTF-8**：直接按字节比较时 `左腕` 永远匹配不上，
   所有映射骨都退化成「静止轨道」。骨名必须解码（`ReadShiftJisName` → `MultiByteToWideChar`
   932 → UTF-8）；morph/IK 名不参与匹配可以照原样读。
2. **文档里的模型名也是 Shift-JIS**：`document.dump()` 会因非法 UTF-8 抛异常并把进程打死，
   必须用 `json::error_handler_t::replace`。
3. **大腿的源链是两根骨**：`Bip001-L-Thigh` = `下半身` 与 `左足` 相乘（右侧同理），
   不是单根。写成「下半身 乘 下半身」时骨盆以下整条腿都错，而计数和其它骨都对得上。
4. **`crot_cap`（未修正的 component 链）要对，但「修正后的 local」要用修正后的父链**：
   方向修正里，`have` 用 `crot_cap[i]`（快照）测量；而 `rest_local[i] = conj(cp_rest)·corrected`
   与 `rest_crot[i] = corrected` 里的父是**已修正的** `rest_crot[p]`。这两者在参考实现里是
   同一个数组随着循环前进，分开写时容易把父修正叠两次。

### 阶段一：把文件跑通（能加载、能动）—— 已完成

**目标**：选一个 VMD → 用当前角色的骨架导出 → 出一份能被插件 `load` 的动作 JSON，
身体动作正确。头发/裙摆还是刚性的（不摆），手指是最粗的映射。

包含（规格书 §3.1–3.6、3.9、3.10）：

| # | 单元 | 说明 |
| --- | --- | --- |
| 1 | **VMD 二进制解析** | header/版本 → 模型名（Shift-JIS）→ 骨关键帧表（name 15B、frame、pos×3、quat×4、64B 插值）→ 其余段走完做 `walkExact` 自检。落成内存结构，不落盘。 |
| 2 | **PMX 骨骼表读取** | 随包的 `reference-pmx.json`（只读 `name/index/parent/position/tailBone?/tailOffset?`） |
| 3 | **骨架导出读取** | 插件已有的 `baseLocal/refLocal/bones[index,parent,name]`，校验父在子前 |
| 4 | **采样器** | 端点保持 + 键间 slerp + 位置线性（§3.4）；**绝不把未命中当成单位四元数** |
| 5 | **骨名映射** | 22 条静态表 + 手指链（阶段一可先用 legacy 位置映射，须在 `diagnostics` 标注） |
| 6 | **输出数学** | `delta = A·Q·A⁻¹`、`local = conj(Cp(rest))·delta·Cg(rest)`、6 位小数；根位移按 MMD 单位增量 + 轴变换 |
| 7 | **每骨有轨道** | 没被驱动的骨输出恒定轨道（§3.9），否则运行时会把它钉在场景里 |
| 8 | **文档头** | `kind/targetMesh/fps/frameCount/firstFrame/rootBone/rootTranslationUnit/...`（§3.10） |

**不含**：手指长度对齐、方向修正、手链 rest 选择、弹簧次运动、扭转骨 —— 这些留阶段二。

**验收**：与 Python 版对**同一份骨架+VMD**输出逐帧逐骨比对（见下方验收方法），
阶段一允许只对上「非 §3.3 修正骨」的轨道。

### 阶段二：把动作做对（发丝/手指/扭转）

| # | 单元 | 说明 |
| --- | --- | --- |
| 1 | **手指链长度对齐** | 目标 4 节按累计长度对齐 MMD 3 节，指尖也有轨道；多出的掌骨输出静止轨道（§3.2） |
| 2 | **方向修正** | `rest_crot = qswing(have, want)·base`，>90° 放弃该骨；`Finger/Hand/Foot/Toe` 豁免（§3.3） |
| 3 | **手链 rest 选择** | 捕获/参考两候选各算最大修正量，取更小且 ≤90°（小吱 126° vs 70° ⇒ 参考） |
| 4 | **弹簧次运动** | 4 子步、刚度 200、阻尼 16、drape 0.15、inertia 0.5、lag 上限 25°、加速度钳 3 g（§3.7） |
| 5 | **发丝通用规则** | `SWING_HAIR_WORDS` 命中 ⇒ 只弯不拧（§5.1） |
| 6 | **头饰骨名表** | `HEAD_ORNAMENT_BONES` 子串命中 + 整棵子树 ⇒ 刚性（§5.2） |
| 7 | **扭转骨** | 从肢体 delta 里取轴上的扭转角 ×weight(0.5/0.25)，硬钳 ±0.35 rad（§3.8） |
| 8 | **diagnostics** | `mappedBones/directionCorrected/directionSkipped/secondaryBones/twistBones/rigidTracks/unmappedVmdTracks` 全写，用来跟 Python 版对数 |

**阶段二完成（2026-09-14，C++ 已与 Python 版对齐）**

| 组 | 轨道数 | 与 Python 版的最大差 |
| --- | --- | --- |
| mapped（映射骨） | 60 | 分量差 `0.000e+00`（逐位相同） |
| rigid（每骨有轨道） | 144 | 旋转差 `0.00000°`（121 条逐位 + 23 条仅 6 位舍入） |
| spring（次级弹簧）+ twist（扭转骨） | 135 + 16 | 旋转差 < `0.0001°`（全部落在参照自身 6 位舍入内） |
| rootTranslation | 2368 帧 | `0.000e+00` |

验收命令：`anomaly-mmd2bip.exe … --out cpp.json` 后 `compare_motion.py base.json cpp.json` →
`shared tracks compared: 355; over tolerance (1e-06): 0`。

**阶段二踩过的四个坑（都修好了，别再犯）**

1. **根运动必须写回根骨的位置**：`センター` 的绝对位置 ×`unitScale`×轴变换要写进根骨的
   translation，位置链才有角色位移（`mmd2bip.py:1244-1246`）。只把它输出成 `rootTranslation`
   会让位置链静止 → 弹簧的加速度项恒为 0 → 裙摆/发丝「很安静」。这个 bug 还会伪装成
   「弹簧不稳定」：`dt=1/30` 下二阶差分把 1e-7 的差放大 900 倍，再被弹簧的指数增长放大到 179°。
2. **刚性轨道的取值是 `rest_local`（refLocal），不是 `baseLocal`**：写错会让头环子骨差 0.155°。
3. **扭转骨（16 根）必须单独一趟**：被 `is_secondary` 的 `twist` 关键字排除后，它们会落到刚性通道，
   肘/膝的网格扭转就没了。
4. **比较器必须先归一化再算夹角**：参照文件是 6 位舍入的，`2*acos(|q1·q2|)` 对相同但非单位化的
   四元数会报 0.155° 的假差（那是脚本 bug，不是转换器 bug）。

**为什么弹簧能对齐**：弹簧是显式积分器（`d' = w×d`、`w' = K(d×target) - D·w`），线性化后是
`δ'' + Dδ' - Kδ = 0`，D=16、K=200 时有一个正根（约 +8.2/s），即**指数增长**（实测每帧 ×1.3）。
所以它对输入误差是混沌敏感的：任何一处 1e-7 的差都会在 50 帧内放大成 5°、几百帧后差 180°。
**唯一的验收方式就是让每一帧的输入逐位相同**——这也是第 1 条坑必须修的原因；修好之后 151 条
弹簧/扭转轨道全部落在 1e-4° 以内。

**IK 追加（2026-09-14）**：源模型自身的 IK 已实现（§3.11），`--ik` 默认 1。回归护栏：
`--ik 0` 与加 IK 之前的基线**逐位相同**（355 条轨道 ≤1e-6、`rootTranslation` 0.000e+00）；
`--ik 1` 下 Python 与 C++ 对 `fktn-001`（2368 帧）和 `主角`（300 帧）都是 **355/355 ≤1e-6**，
连每条链的 gap 数值都一致。解析验收脚本 `ik_solver_check.py` 单独断言可达/不可达/膝盖方向/限角。

### 验收方法（每阶段都做）

1. **基准**：用 Python 版对同一份骨架+VMD 生成基准文件（现有产物可直接复用：
   `pose-motion-xiaozhi-autohair.json`、`pose-motion-xun.json`）。
2. **逐帧逐骨比对**：`rest` 与 `bones` 全轨道，容差 ≤ 1e-6（6 位小数舍入后应**完全相等**）；
   `rootTranslation` 容差 1e-6。
3. **对账脚本要独立**：比较器自己读两个 JSON 直接比数，**不允许调用转换器的内部函数**
   （历史上就是因为「验证脚本与实现共用取值假设」而漏掉了稀疏关键帧抽动的 bug）。
4. `diagnostics` 的计数、`frameCount/firstFrame/fps/nativeFps/targetMesh/rootBone` 必须一致：
   `mappedBones / directionCorrected / directionSkipped / secondaryBones / twistBones /
   rigidTracks`。
5. 附赠自检：`coverage.py`（每骨有轨道）、`track_amplitude.py`（幅度）。
6. **比四元数夹角前先归一化**（见阶段二坑 4），否则会得到假差异。

## 7. 内置管线设计（游戏内 UI）

```
[选择动作文件…]  →  VMD 解析（内存）  →  读取/导出当前角色骨架  →  retarget  →  写
<插件数据目录>/motions/<角色mesh>-<vmd名>.json  →  自动 load
[载入动作] [播放] [暂停] [卸载动作]（现有面板不动）
```
- 面板只加一个按钮（选择并转换）+ 一行进度/状态；**不暴露任何转换参数**。
- 线程：转换是纯 CPU、可能数秒；不得在渲染回调里同步执行（`.agents/architecture.md`）。
  建议放到插件自己的 worker（现有 `mesh_scan_running` 那套「请求标志 + Update 里分步」的
  模式可以复用）；进度写进状态行/日志。
- 失败必须可见：写进现有 `LogDiagnostic`（`betterpose …` 前缀），别静默。

## 8. 随包数据与文件位置

- `plugins/BetterPose/data/reference-pmx.json`：**只带骨骼表**（用户已确认）——从
  `学马仕 藤田琴音 常服.pmx`（495 骨）筛出 `name / index / parent / position /
  tailBone? / tailOffset?` 六个字段，**不含网格、贴图、材质**，因此与游戏资源分发无关。
- 中间产物（VMD 解析结果、骨架导出）不落盘，只在内存里传递。
- 生成的动作文件写插件数据目录（`storage` API），不再走 `D:\pose-motion-*.json` 这种路径。

## 9. 待确认 / 开放问题

1. **头环/头饰 = 骨名表（已全部收口，2026-09-14）**：`HEAD_ORNAMENT_BONES` 默认生效，
   `--no-sim-names` 临时加。表内 5 条子串覆盖三个角色：小吱 `Bn_m_headProp`（1 骨）、
   浔 `Bn_m_headProA`/`Bn_m_headProB`（4 骨）、伊洛伊 `Bn_m_headLineA`/`Bn_m_headLineB`
   （30 骨：A 11 + B 19，已实测 30 根全进 `rigidBones`，次级骨 163→135）。
   **条目按子串匹配**（一条覆盖一整组），不是全名精确匹配。2026-09-14 追加 `hat`（九原的大帽子），
   并且匹配统一按**小写**比较（C++ 侧原来区分大小写，已改成与参照一致的大小写无关）。
2. ~~手指 roll~~ → 已定：**当前不可达**（手链被排除在方向修正外，三种取值输出 sha1 相同），
   移植**原样保留成预留参数**，不做逐角色配置（§5.3）。
3. **浔的飘带/小飞饰是否也要固定**（待用户确认）：实测在摆的是
   `Bn_m_piaodaiF_001`（165°）、`Bn_l_headPiaoC_002`（155°）、`Bn_m_flyC_001`（146°）、
   `Bn_l/headPiaoC_*`、`Bn_m_headPiaoD_001`（56°）、`Bn_m_flyB_001`（33°）、
   `Bn_m_headFlyA/B_001`（25°）、`Bn_l/r_earing_001`（178°）。这些是飘带类的柔性件，
   目前保持模拟（会摆）。
4. **参考模型只带骨骼表**（用户已确认）：随包只放 §8 的骨骼表（骨名/索引/父子/绑定位置/
   尾巴），**不含网格、贴图、材质**。最小字段集 = `name / index / parent / position /
   tailBone? / tailOffset?`（`mmd2bip.py` 只读这些）。
5. 转换耗时与并发：一份 2368 帧 / 253 骨的动作在当前 Python 版约数秒；C++ 会更低，
   但仍要确认是否需要对「转换中禁止播放/切角色」加互锁。
6. **IK（`左足ＩＫ` 等）已实现（2026-09-14）**：4 条链进内置数据表，Python 与 C++ 两边
   都用 `--ik {0,1}`（默认 1），开关轨（`on/off`）也两边都读。`主角.vmd` 的腿现在会跟随
   （300 帧：左右腿 gap 7.24/6.64 → 0.61/0.46，残余＝模型的不可达量）；对已经验收过的
   `fktn-001` 则是一处**小修正**（6 条映射骨最多 0.23 分量、脚趾配件骨最多 26°）——MMD 本身
   也会这么修，但要游戏内再看一眼。7 个样例 VMD 现在**全部 walk 精确**（尾部布局按 §3.11 修正）。
   仍未做的：IK 骨的**旋转**——实测它其实是**大量使用且必须用**的（见 §3.11），而实现里走通用
   FK 已经覆盖，不需要额外代码。
7. **躯干不再跟髋一起倾（2026-09-15）**：目标的骨盆只有一根，同时扮演 MMD 的 `腰`（挂躯干）和
   `下半身`（挂腿）。`下半身` 装在骨盆上时臀/腿对、但躯干底座被同一旋转带偏（rigoutput 实测
   骨盆→脊椎方向差最多 40.55°、头相对髋位移 5.5 cm，集中在髋动的帧）。修法是**三件套**：
   (a) `Bip001-Spine` 的源按**左乘**乘回 `下半身⁻¹`（旧「inverse」右乘只有两者可交换时才对，
   表扩展到第 6 项 `prefix`）；(b) 脊柱底座按帧补一个**局部平移** `t = C⁻¹·H⁻¹·C·rest`
   （C=骨盆 component rest，H=`下半身` 世界 delta），经新文档字段 `boneOffsets` 交给插件；
   (c) 脊柱链一一对应：`Spine←上半身 / Spine1←上半身1 / Spine2←上半身2`（原来丢掉 `上半身1`、
   `Spine2` 不驱动）。修后躯干动态误差 5.5 cm → <0.1 cm，腿/脚/根运动不受影响（feet_check 逐项
   与基线一致）。`boneOffsets` 是 `{骨名: [[x,y,z]×frameCount]}`，单位 cm、绝对局部平移、可选字段。
8. **目标侧腿部 IK（2026-09-15）**：手臂/躯干可以映射源**旋转**，腿不行——源模型的腿比例
   （大腿/小腿比、`腰→左足` 的髋偏移）与目标骨架不同，烘焙腿会让脚踝偏好几厘米、骨盆比源低
   ~6 cm（"身子矮、往前栽、脚不在圆心"）。段长度本身是匹配的（源 9.208 单位 × `unit_scale`
   = 目标 `Calf+Foot`），差的是"腰→髋"这段在目标上不存在。
   **做法**：不再烘焙腿，改为**在目标骨架上重解**——髋放到源髋的位置（映射+缩放）、脚踝瞄源
   脚踝、用目标自己的 `Calf`/`Foot` 长度做两骨解算弯膝盖。这正是 MMD 播放时的行为（所以
   "换任何模型都一样"）：腿长差变成膝盖弯曲，而不是身体错位。
   - 解算在**"相对源腰(腰)"的坐标系**里做（`hip_rel`/`goal_rel`/`knee_ref_rel`），与转换器
     内部如何摆放根无关；髋的局部偏移按**骨盆当前 component 旋转**反解，随身体转动（世界系固定
     偏移会随骨盆转整圈，实测引入 12–15 cm 动态摆动，已否决）。
   - 膝盖弯曲平面取**源膝盖**定义的平面（死用世界轴会侧弯几十度）。
   - 骨轴 roll 用"从映射姿态最小 swing"保留；脚的**绝对朝向**保持不变。
   - 髋偏移写进 `boneOffsets`（插件端按目标自身 rest 摆位，不写就少这段）。
   - 验收：髋/脚踝动态误差 **6.21/8.81 → 0.00/0.00 cm**（rigoutput）、fktn 同样 0.00/0.00；
     骨盆↔脚踝（前后/高度）逐帧 **0.00 cm**；膝盖残差 2–4 cm（两模型腿段比例不同，不可消除）；
     躯干/手臂不动（躯干 ≤0.5 cm、手臂 ≤0.35°）；Python ≡ C++ 355/355 + `boneOffsets` 0.0。

9. **腿挂在目标自己的髋上（2026-09-15）**：第 8 条的腿 IK 原先把「源 `腰`」当解算原点，并把
   「源 腰→髋」的落差写进 `boneOffsets` 当髋的局部平移。那个落差是**源模型自己的常量**
   （作者模型 0.676 单位 ≈ 5.9 cm；用户的 `Miku_Hatsune.pmd` 2.49 单位 ≈ 21.3 cm），于是换参考后：
   - 目标的腿被挂到自己髋下方 21 cm ⇒ 同一帧脚踝 **+3.3 → −10.9 cm**（下沉约 14 cm、脚穿地）；
   - 想用「源髋绕骨盆的位移」补偿也不行——它和**目标自己的 rest 髋偏移**相加不再是同一个偏移的
     刚体旋转，两个髋被推向外侧（实测髋间距 **14.8 → 30–45 cm**，用户在游戏里看到「两大腿开得很扩」）。
   **最终做法**：解算原点取**源的髋（左足/右足）**，`hip_rel = 0`，`goal/knee_ref` 都相对髋取；
   髋的局部平移**完全不覆盖**（保留目标自己的 rest），`boneOffsets` 只剩 `Bip001-Spine`（`[off 1]`）。
   - 髋间距因此**恒等于目标骨架自己的 rest 值 15.48 cm**（旧版用源偏移给 14.77/13.62，也是常量）；
   - 腿的形状 = 源 `髋→膝→踝`，可达性由 `|踝−髋| ≤ 腿长` 天然保证（源腿长 × `unit_scale` = 目标腿长）；
   - 脚踝高度 = 目标 rest 踝高 + 源的抬脚量，两种参考逐帧差 ≤1.2 cm（原来 14–20 cm）：
     帧 0/796/1200/1750/2430 = 10.3/8.9/10.7/18.9/60.4（PMD）对 9.2/8.8/9.8/18.3/64.9（PMX）；
   - `腰` 缺失不再影响腿 IK（PMD 骨架照跑，`legIK 2`）。
   验收（rigoutput，`--feet-anchor 0` = 插件默认）：Python ≡ C++ 在现用参考与 `主角.vmd` 上逐值 0.0
   （113 条映射轨 + `rootTranslation` + `boneOffsets`）。
   **已知残差**：参考换成 PMD 形状（无 `腰`/`上半身1`/`上半身2`，`下半身` 直挂 `センター`）时，
   C++ 的骨盆方向修正能取到方向（回退用 `下半身`），Python 的目标侧方向查找返回 None ⇒
   4 条轨（Pelvis/Spine/L·R-Thigh）差 ≤1.9°、`boneOffsets` ≤0.15 cm（加 `下半身` 回退前是
   24 条轨 + 2.03 单位根运动差）。游戏跑的是 C++ 侧。
   配套：`mmd2bip.py` 的骨盆映射加了 `腰 → 下半身` 回退（否则整条映射被丢、骨盆变刚性、少 1 根映射骨）。
10. **参考模型的骨架比例决定姿势（2026-09-15）**：同一份 `rigoutput.vmd` 换参考模型，目标角色
   （伊洛伊）手腕相对头骨的高度会整体翻转：现用参考 `学马仕 藤田琴音 常服`（拆包骨架，`センター`
   在脚底 y=0 / 肩 14.85 / 头骨 16.10）给出 **+5.02 / +8.37 cm（头骨上方，帧 796/1750）**，用户的
   `Miku_Hatsune.pmd`（标准 MMD 骨架，`センター` 在胯 y=8 / 肩 16.13 / 头骨 16.91）给出
   **−10.64 / −8.37 cm（头骨下方）**——与用户「游戏里手高过头、MMD 里手在头下」的观察一致。
   把参考里的 `上半身2/上半身1` 改名让轨道失配后目标仍是 +8.28 / +4.04 ⇒ 主因是**骨架 rest 比例**，
   不是胸链轨道。**结论：参考模型必须是 MMD 里实际播放用的那个模型**（或动作作者的模型）。

## 附 A. 现有交付物（验收基准）

| 文件 | 角色 / 参数 | 规模 |
| --- | --- | --- |
| `D:\pose-motion-jiuyuan.json` | 伊洛伊，`--no-sim headLine --sim-swing-only hairF_,hairA_,hairB_` | 33.5 MB / 344 tracks / 60 mapped |
| `D:\pose-motion-nanali.json` | 娜娜莉，全默认 | 23.3 MB / 253 tracks / 52 mapped |
| `D:\pose-motion-zankou-v3.json` | 残虹，默认 | 37.1 MB / 385 tracks / 60 mapped |
| `D:\pose-motion-xiaozhi-headfixed.json` | 小吱（mesh `0x19ED057780`），`--no-sim Bn_m_headProp_001,Bn_m_hat_001` | 35.0 MB / 379 tracks / 60 mapped |
| `D:\pose-motion-xiaozhi-autohair.json` | 同上 + **自动发丝只弯不拧**（推荐用这份验收） | 35.1 MB / 379 tracks / 60 mapped |
| `D:\pose-motion-xun.json` | 浔（mesh `0x181ABE5560`，527 骨），内置骨名表（4 件 headPro 刚性）+ 自动发丝 | 2368 帧 / 527 tracks / 60 mapped |
| `D:\pose.skeleton.json` | 当前导出槽（**小吱，379 骨**，含 `refLocal`） | — |
| `D:\Anomaly-main\.local\vmd2anim-ref\{pmx1,fktn}.json` | 参考模型 / 动作解析 | 495 骨 / 255 轨道 |

## 附 B. 诊断脚本（迁移后可删，迁移期用来对账）

`coverage.py`（每骨有轨道）、`track_amplitude.py`（幅度）、`group_tracks.py`（轨道分组）、
`direction_audit.py`（方向修正审计）、`pmx_dump.py` / `vmd_dump.py`（解析器 + `walkExact` 自检）。

本次新增（`.local/vmd2anim-ref/`，不入库）：`head_chains.py`（头骨下每条链的模拟/刚性判定）、
`why_moves.py`（某根骨为什么会动：映射/轨道/次级）、`ring_signature.py`（链形状签名）、
`twist_check.py`（发骨偏差中「拧」的占比）、`twist_compare.py`（两版动作的拧/摆对比）、
`hair_rule.py`（发丝/头饰关键词分类在你手上所有骨架上的命中统计）。
