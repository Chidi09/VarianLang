# Lumen example app

A minimal file-based Lumen app — the Next/Nuxt-style workflow, batteries included, zero dependencies.

```
pages/
  index.lumen   → /          (an interactive counter)
  about.lumen   → /about
```

## Run it

```sh
vn dev examples/lumen_app/pages 8090
# then open http://localhost:8090
```

Or scaffold a fresh one anywhere:

```sh
vn lumen new myapp
cd myapp
vn dev pages
```

## How it works

- A `.lumen` file is a component: `<template>` markup + a Varian `<script>`
  (`state()` plus event handlers).
- `{{ }}` interpolates (HTML-escaped by default); `@click="inc"` binds an event.
- `vn dev` compiles every page, maps file names to routes, and serves them with
  the **server-driven live** model: the server owns state and renders HTML, a
  tiny client runtime (**Lumen JS**) forwards events over a WebSocket, and the server pushes back a
  minimal DOM patch. No build config, no `node_modules`, no hydration mismatch.
- Handler errors surface as a branded in-browser error overlay during dev.
