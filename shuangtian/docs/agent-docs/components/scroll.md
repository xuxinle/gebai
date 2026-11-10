---
title: 滚动族 · ScrollView/ScrollBar
summary: 内容超视口就套 ScrollView；它负责裁剪、滚动条与滚轮，快捷键与焦点仍归应用。
---

# 滚动（scroll）

## 选型

| 情况 | 用 |
|---|---|
| 一列内容超视口 | `ScrollView`（子节点按列堆叠、超出裁剪、右侧自动出 `ScrollBar`） |
| 自绘面板里只要一根滚动条 | `ScrollBar`（宽 8px，hover/拖拽时 10px） |
| 二维缩放/平移一块画布 | `Viewport`（见布局族） |
| 行数极多的列表 | `Table`/`Tree` 自带虚拟滚动，不要塞进 `ScrollView` |

## 用法要点

- `ScrollView` 只做**纵向**；横向滚动条不在它职责内。
- 滚轮事件的归属由 `Wheel` 路由决定：**嵌套滚动时只有最内层消费**，否则一滚全动。
- 滚动容器的内容必须**可以无界测量**（`measure` 给的高度约束要按内容算），否则测高后裁剪会截断内容。

## 坑

| 坑 | 说明 |
|---|---|
| 内部件被裁后悬垂 | 曾出现"`ScrollView` 内部件被裁→悬垂"的缺陷；把元素移出容器前先从父级摘干净 |
| 在滚动容器里放绝对定位浮层 | 会被裁剪；浮层挂 root overlay |
| 把 `ScrollView` 当虚拟列表 | 它不回收元素，几千行会同时存在 |
| 每帧重设 `scroll` 位置 | 会打断用户拖动；只在程序性跳转时写 |

## 本族清单

`ScrollView` · `ScrollBar`

- 逐个：`components/scroll/ScrollView.md` · `components/scroll/ScrollBar.md`

## 更细

- 视口与增量重绘（损坏区）：`DESIGN.md` §4.2.8.1
- 二维视口：`components/layout.md`（`Viewport`）
- 滚轮路由：`include/st/ui/wheel.hpp`
