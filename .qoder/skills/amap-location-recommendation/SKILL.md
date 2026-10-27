---
name: amap-location-recommendation
description: 基于高德开放平台 Web 服务 API 做区位推荐与开店选址评估。给定城市和业态时自动发现候选商圈/区位并打分排序；也可对用户给出的多个候选地址做竞品密度、客流配套、可达性对比。当用户提到区位推荐、开店选址、选址评估、商圈分析、候选地址对比、某城市开某类店选哪里时使用。
---

# 高德区位推荐

## 先读这三条

1. 需要环境变量 `AMAP_KEY`（高德 Web 服务 Key）。没有就先向用户要，让用户自己 `export`；**不要把 Key 写进仓库、报告或提交**。
2. 所有数字来自高德 POI/交通 API，是**代理指标**：POI 数量不等于真实客流，也不含租金、人口、销售额。向用户汇报时必须保留这个口径。
3. API 失败、配额不足或地址解析不到时，如实说明并停止，不要编造候选区位或分数。

## 快速开始

```bash
export AMAP_KEY="你的高德 Web 服务 Key"   # PowerShell: $env:AMAP_KEY="..."

# 模式一：城市 + 业态，自动发现候选区位
python .qoder/skills/amap-location-recommendation/scripts/recommend.py discover \
  --city 杭州 --business coffee --top 5

# 模式二：用户已给出候选地址，做对比排名
python .qoder/skills/amap-location-recommendation/scripts/recommend.py score \
  --city 杭州 --business 奶茶 \
  --candidates "湖滨银泰in77,武林广场,钱江新城"

# 机器可读 + 可视化报告
python .../recommend.py discover --city 成都 --business 便利店 --json --html report.html
```

`--html` 会生成带排名卡片和高德地图链接的报告；若额外设置 `AMAP_JS_KEY`（或 `AMAP_JSAPI_KEY`，需要时再配 `AMAP_JS_SECURITY_CODE`），报告内会直接渲染地图。

## 两种模式

### discover：自动发现

1. 按“购物中心 / 写字楼 / 地铁站”三类锚点在城市内检索 POI。
2. 按约 600m 网格聚合，按锚点权重（购物中心 3、写字楼 2、地铁站 1）排序，并在 1.2km 内去重。
3. 对每个候选区位统计周边 POI、竞品与交通态势，输出排名。

适合“在杭州开咖啡店选哪里”“成都便利店哪些商圈值得看”。

### score：候选对比

用户已经给出 2~10 个地址/商圈时使用。逐个地理编码后走同一套指标，输出排名、分差、依据与风险。

适合“这三个商场哪个更适合开奶茶店”。

## 常用参数

| 参数 | 说明 | 默认 |
|---|---|---|
| `--business` | `coffee` / `tea` / `restaurant` / `convenience` / `gym` / `generic`，也接受“咖啡”“奶茶”等中文或自定义关键词 | `coffee` |
| `--radius` | 周边统计半径（米），200~5000 | `800` |
| `--top` | 输出前 N 个，`0` 为全部 | discover 5 / score 全部 |
| `--max-candidates` | discover 参与打分的候选数上限 | `8` |
| `--anchor-pages` | discover 每类锚点检索页数（每页 25 条） | `2` |
| `--cache-dir` | 缓存 API 响应，重复分析同一区域时省配额 | 关闭 |
| `--json` | 输出 JSON | 关闭 |
| `--html` | 输出 HTML 报告路径 | 关闭 |

## 评分口径

综合分 = 需求 55% + 竞争 20% + 可达性 25%，三项均为 0~100。

- **需求**：周边购物中心、写字楼、住宅区、学校数量，按业态权重加权。单类计数用对数压缩，达到 `scripts/recommend.py` 里的 `CATEGORY_CAPS` 即视为满分。
- **竞争**：餐饮/咖啡/奶茶按“适度聚集最好、过度饱和变差”打分；便利店/健身房按“竞品越少越好”打分。
- **可达性**：地铁站 50% + 公交站 20% + 实时交通态势 30%。

业态权重、竞争模式、饱和度都在 `scripts/recommend.py` 的 `PROFILES` 中，需要调整时直接改表，不要改评分函数。

## 向用户呈现的要求

1. 先给 Top 3 结论和分差，再展开依据；不要只丢一张表。
2. 每条推荐至少给出：核心客流设施数量、竞品数量、交通态势、主要风险。
3. 明确说明“POI/交通为代理指标，落地前需线下核验租金、客流与证照条件”。
4. 用户问“为什么”时，引用 `evidence.poi_counts` 与 `evidence.competitors`，不要用模型常识补数字。
5. 分差小于 3 分时提示“差距在误差范围内，建议并列考察”。

## 错误处理

| 现象 | 处理 |
|---|---|
| 未设置 `AMAP_KEY` | 让用户配置环境变量，并给出申请地址 |
| `INVALID_USER_KEY` / `USERKEY_PLAT_NOMATCH` | Key 错误或未勾选 Web 服务，让用户检查控制台 |
| `DAILY_QUERY_OVER_LIMIT` / `CUQPS_HAS_EXCEEDED_THE_LIMIT` | 配额或 QPS 超限，建议 `--cache-dir` 复用结果，或换时间再跑 |
| 地址解析失败 | 让用户补充城市或换更具体的地址，不要猜坐标 |
| 交通态势失败 | 脚本会自动降级为中性分并写入 `warnings`，向用户说明即可 |

## 资源

- `scripts/recommend.py`：检索、聚类、打分、Markdown/JSON/HTML 输出；`--selftest` 可离线自测。
- `assets/report_template.html`：HTML 报告模板。
- `references/amap-api.md`：高德接口、POI 编码、配额与错误码。

## 设计参考

接口用法参考魔搭 `@AMap-Web/amap-lbs-skill`（官方综合服务），指标与评分维度参考 `huangby0305/store-site-analysis`（商铺选址）；本 Skill 增加了候选区位自动发现、聚类去重和离线自测。
