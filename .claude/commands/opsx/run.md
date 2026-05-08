---
name: "OPSX: Run"
description: 编排子 Agent 执行实现→测试→验证循环，直到所有任务通过验证
category: Workflow
tags: [workflow, orchestration, subagent]
---

编排三个子 Agent 完成"实现→测试→验证"循环。主 Agent 是纯编排器：不写代码、不跑测试、不做验证，只做信息路由和状态决策。

**子 Agent 架构:**

```
主Agent (编排器)
  ├── Implementer  — 唯一能写代码的 Agent
  ├── TestRunner   — 只跑测试，不写代码，不做分析
  └── Verifier     — 只做需求对齐检查，不写代码，不跑测试
```

**关键规则:**
- 子 Agent 之间**不能通信**，所有信息必须经主 Agent 中转
- 主 Agent 每次派发子 Agent 前，必须将上下文组装成**完整的"中转包"**
- 主 Agent 不修改代码、不运行测试、不执行验证

**输入:** `/opsx:run <change-name>`，如省略则从对话上下文推断。

---

## 步骤

### 1. 确定变更

如果提供了名称，使用它。否则：
- 从对话上下文推断
- 如果只有一个活跃变更，自动选择
- 如果模糊，运行 `openspec list --json` 并使用 **AskUserQuestion** 让用户选择

宣布: "使用变更: **<name>**" 以及如何覆盖。

### 2. 获取变更状态和产物

```bash
openspec status --change "<name>" --json
openspec instructions apply --change "<name>" --json
```

解析并读取所有 contextFiles（proposal、design、tasks 等）。

### 3. 初始化状态

维护以下运行时状态:

```
current_task_index: 0       # 当前任务序号
modified_files: []           # 本轮已修改文件列表
verify_report: null          # 最近一次验证报告
loop_count: 0                # 当前任务重试次数
state: "IMPLEMENT"           # 当前状态机状态
```

展示进度: "任务进度: 0/N，开始执行。"

### 4. 进入编排循环

按以下状态机循环，直到所有任务完成且验证通过:

```
          IMPLEMENT
              │
              ▼
          TEST ──(失败)──→ FIX_TEST ──→ TEST
              │
          (通过)
              │
              ▼
          VERIFY
              │
      ┌───────┼───────┐
      ▼       ▼       ▼
   无严重   有严重   有警告
   无警告    │       │
      │      │       │
      ▼      ▼       ▼
   完成   FIX_VERIFY ──→ TEST
```

**退出条件:**
- TEST 通过 AND VERIFY 无严重+无警告 → 当前任务完成，取下一任务
- 所有任务完成 → 进入收尾

**单个任务重试上限:** 5 次。超过后暂停并询问用户。

### 5. 状态: IMPLEMENT — 派发 Implementer

**时机:** 每个新任务开始，或从 FIX_TEST/FIX_VERIFY 状态转入。

**主 Agent 组装中转包 A:**

```json
{
  "mode": "implement | fix_test | fix_verify",
  "task": {
    "id": "<task-id>",
    "title": "<任务标题>",
    "description": "<从 tasks.md 提取的完整任务描述>",
    "acceptance_criteria": ["<验收条件1>", "<验收条件2>"]
  },
  "coding_context": {
    "files_to_modify": ["<从任务描述推断的文件>"],
    "related_files": ["<相关但不需要修改的文件>"],
    "constraints": [
      "必须通过编译 (cmake --build build --target backend_all)",
      "遵循现有代码风格和命名约定",
      "<来自 design.md 或 proposal 的额外约束>"
    ]
  },
  "failure_context": {
    "source": "<TestRunner | Verifier | null>",
    "failing_tests": ["<仅 fix_test 模式>"],
    "error_snippets": ["<仅 fix_test 模式>"],
    "verify_issues": [
      {"level": "严重|警告", "desc": "<问题描述>", "file": "<文件:行号>"}
    ]
  },
  "project_state": {
    "modified_files": ["<本轮已修改的文件列表>"],
    "change_dir": "openspec/changes/<name>/"
  }
}
```

**派发命令:**
```
Agent(
  description: "实现任务: <task-id> <title>",
  subagent_type: "general-purpose",
  prompt: <将中转包 A 格式化为自然语言指令，参见下方"派发提示词模板">
)
```

**收集返回值:** 记录 `modified_files` 和实现摘要。转入 TEST 状态。

### 6. 状态: TEST — 派发 TestRunner

**时机:** IMPLEMENT/FIX_TEST/FIX_VERIFY 完成后。

**主 Agent 组装中转包 B:**

```json
{
  "scope": {
    "type": "all | tag | name",
    "value": "<根据变更内容推断测试范围>"
  },
  "build": {
    "source_dir": ".",
    "build_dir": "build",
    "target": "backend_all"
  },
  "context_for_diagnosis": {
    "changed_files": ["<本轮修改的文件>"],
    "impl_summary": "<Implementer 返回的改动摘要>"
  }
}
```

**推断测试范围:**
- 如果变更涉及 `common/comm/` → scope: tag=shm_pubsub,shm_ring_buffer
- 如果变更涉及 `common/config/` → scope: tag=channel_topology_config,launch_plan_config
- 如果是跨模块变更 → scope: type=all
- 不确定时 → scope: type=all（宁可多跑不漏）

**派发命令:**
```
Agent(
  description: "运行测试验证变更",
  subagent_type: "general-purpose",
  prompt: <将中转包 B 格式化为自然语言指令>
)
```

**收集返回值:** 解析 pass/fail 和错误摘要。
- 如果 **编译失败** → 转入 FIX_BUILD 状态（视为 FIX_TEST 的特例）
- 如果 **测试失败** → 转入 FIX_TEST 状态，将失败信息填入中转包 A 的 `failure_context`
- 如果**全部通过** → 转入 VERIFY 状态

### 7. 状态: VERIFY — 派发 Verifier

**时机:** TEST 全部通过后。

**主 Agent 组装中转包 C:**

```json
{
  "requirement_artifacts": {
    "plan": "openspec/changes/<name>/proposal.md",
    "design": "openspec/changes/<name>/design.md",
    "tasks": "openspec/changes/<name>/tasks.md"
  },
  "changes_since_last_verify": {
    "files": ["<本轮修改的文件>"],
    "summary": "<本任务实现摘要>",
    "previous_report": "<上次验证报告摘要，无则 null>"
  },
  "completed_tasks_in_this_round": ["<task-id>"]
}
```

**派发命令:**
```
Agent(
  description: "验证需求对齐",
  subagent_type: "general-purpose",
  prompt: <将中转包 C 格式化为自然语言指令>
)
```

**收集返回值:** 解析审查报告，提取严重/警告/建议列表。

- 如果**无严重且无警告** → 标记当前任务完成（`- [ ]` → `- [x]`），取下一任务
- 如果有**严重或警告** → 转入 FIX_VERIFY 状态，将问题列表填入中转包 A 的 `failure_context`

### 8. 状态: FIX_TEST / FIX_BUILD / FIX_VERIFY

这些状态都派发给 **Implementer**（唯一能写代码的 Agent），区别在 `mode` 字段:

| 状态 | mode 值 | failure_context.source |
|------|---------|----------------------|
| FIX_TEST | `fix_test` | TestRunner |
| FIX_BUILD | `fix_build` | TestRunner (编译错误) |
| FIX_VERIFY | `fix_verify` | Verifier |

重试计数器 +1。超过 5 次 → 暂停并报告用户。

### 9. 所有任务完成

确认:
- 所有 tasks.md 中的复选框为 `[x]`
- 最终 TEST 通过
- 最终 VERIFY 无严重和警告

展示汇总:

```
## 执行完成

**变更:** <name>
**进度:** N/N 任务完成 ✓
**测试:** 全部通过 ✓
**验证:** 无严重/警告 ✓

所有任务已完成，可以运行 `/opsx:archive` 归档。
```

---

## 派发提示词模板

### 派发 Implementer 的提示词

```
你是一个代码实现者（Implementer）。你的唯一职责是根据任务描述编写/修改代码。

**模式:** {mode}
**变更目录:** {change_dir}

## 任务

**ID:** {task.id}
**标题:** {task.title}
**描述:** {task.description}
**验收条件:** {task.acceptance_criteria}

## 编码上下文

**要修改的文件:** {coding_context.files_to_modify}
**相关文件（参考用，不修改）:** {coding_context.related_files}
**约束条件:** {coding_context.constraints}

{failure_context 区块（仅 fix_test/fix_verify 模式）:}
## 需要修复的问题

来源: {failure_context.source}
{如果是 TestRunner 来源:}
失败用例: {failure_context.failing_tests}
错误摘要: {failure_context.error_snippets}

{如果是 Verifier 来源:}
审查问题:
{逐个列出 verify_issues 的 level + desc + file}

## 项目状态

当前已修改但未提交的文件: {project_state.modified_files}

## 要求

1. 只做任务要求的事情，不要做额外重构或"顺便优化"
2. 修改完代码后用 Bash 验证编译通过: cmake --build build --target backend_all
3. 编译通过后，列出你修改了哪些文件，简要说明改了什么
4. **不要运行测试**，那不是你的职责
5. **不要运行 openspec verify**，那不是你的职责
6. 不要与其他子 Agent 通信，有任何问题通过主 Agent 转达
```

### 派发 TestRunner 的提示词

```
你是一个测试执行者（TestRunner）。你的唯一职责是运行构建和测试，报告客观结果。

## 测试范围

类型: {scope.type}
值: {scope.value}

## 构建配置

源码目录: {build.source_dir}
构建目录: {build.build_dir}
构建目标: {build.target}

## 变更上下文（仅用于理解测试范围，不用于分析代码）

修改的文件: {context_for_diagnosis.changed_files}
实现摘要: {context_for_diagnosis.impl_summary}

## 要求

1. 先配置并构建:
   cmake -S {build.source_dir} -B {build.build_dir} -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build {build.build_dir} --target {build.target}

2. 构建成功后运行测试:
   {根据 scope.type 生成对应的 ctest 命令}
   ctest --test-dir {build.build_dir} --output-on-failure {scope 参数}

3. 报告结果时只包含客观事实:
   - 通过/失败状态
   - 失败用例名称列表
   - 关键错误信息原文摘要（不要添加你的分析）
   - **绝对不要**写"可能是 XX 行有空指针"这类推断

4. **不要修改任何文件**
5. **不要分析代码为什么会失败**，那由其他人负责
6. 不要与其他子 Agent 通信
```

### 派发 Verifier 的提示词

```
你是一个需求对齐验证者（Verifier）。你的唯一职责是对比需求产物和代码变更，产出分级审查报告。

## 需求产物路径

- proposal: {requirement_artifacts.plan}
- design: {requirement_artifacts.design}
- tasks: {requirement_artifacts.tasks}

## 本轮变更

修改的文件: {changes_since_last_verify.files}
实现摘要: {changes_since_last_verify.summary}
已完成的 tasks: {completed_tasks_in_this_round}

{如果有 previous_report:}
## 上次验证报告

{previous_report}

## 要求

1. 读取所有需求产物文件（proposal/design/tasks）

2. **完整性检查:**
   - tasks.md 中当前任务的检查项是否全部完成
   - 如果有需求描述（proposal 中的 Requirements），是否都有代码对应

3. **正确性检查:**
   - 实现是否与 design.md 中的设计决策一致
   - 是否处理了边界情况和错误路径（如 design 中有描述）

4. **一致性检查:**
   - 新增代码是否符合项目现有模式
   - 文件命名、目录结构、接口风格是否一致

5. 输出**分级报告，每个问题必须指定具体文件和行号**:

   **严重（必须修复）:**
   - 任务未完成（tasks.md 中该任务要求的功能缺失）
   - 与 design.md 的架构/接口决策明显矛盾
   - 缺少关键错误处理（如 design 中明确要求的）

   **警告（应当修复）:**
   - 实现与 spec 细节有偏差
   - 缺少 design 中提到的边界条件处理
   - 新增代码使用了与项目现有模式不一致的方式

   **建议（可选修复）:**
   - 代码风格轻微不一致
   - 命名不够清晰（但功能正确）
   - 可以更简洁的实现方式（不影响功能）

   **不确定时降级处理:** 拿不准严重还是警告 → 标警告；拿不准警告还是建议 → 标建议

6. **不要运行测试**
7. **不要修改任何文件**
8. 不要与其他子 Agent 通信

## 输出格式

```
## 验证报告: <change-name>

### 任务: <task-id> <title>

| 维度 | 状态 |
|------|------|
| 完整性 | <结果> |
| 正确性 | <结果> |
| 一致性 | <结果> |

### 严重
- [ ] **<文件:行号>** — <问题描述> → 修复建议

### 警告
- [ ] **<文件:行号>** — <问题描述> → 修复建议

### 建议
- [ ] **<文件:行号>** — <问题描述> → 修复建议

### 结论
<根据严重/警告/建议数量给出明确结论>
```
```

---

## 防护规则

- **唯一写权限:** 只有 Implementer 可以修改文件。主 Agent 即使发现明显的拼写错误也不直接改
- **透明中转:** 主 Agent 从子 Agent 收到的信息必须原样传递，不添不减
- **超限处理:** 单个任务重试 5 次仍不通过 → 暂停，展示失败历史，询问用户
- **编译失败视为测试失败:** build_fail → FIX_BUILD → 走 fix_test 路径
- **验证前必须测试通过:** 不允许跳过测试直接验证
- **保持 Agent 隔离:** 每个子 Agent 的 prompt 中必须明确"不要与其他子 Agent 通信"
