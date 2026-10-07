# Time-Travel Debugger

A Data Structures and Algorithms project implemented in C++.

## Project Overview

The Time-Travel Debugger executes programs written in a small custom language called C-- and records their execution history as snapshots.


## Phase 1: Server

The server processes an existing `source.bin` file through these stages:

1. Validate function declarations.
2. Generate `resolve.bin` and resolve function call destinations.
3. Execute instructions using a custom call stack.
4. Record execution snapshots in a doubly linked timeline.
5. Serialize the timeline into `session.tdbg`.

## Supported Instructions

`func`, `func_end`, `call`, `set`, `add`, `sub`, `mul`, and `div`.

## Main Data Structures

- Linked stack for active function calls.
- Doubly linked list for execution snapshots.
- Arrays for function lookup and unresolved call patches.
- Dense index for locating snapshots in the output file.

## Current Status

The repository contains the provided server template and initial project documentation. The implementation is not yet complete.

## Progress Report

Development milestones, checks, and learning notes are recorded in [PROGRESS.md](PROGRESS.md).

## Build and Run

Build and execution instructions will be added after the development environment is configured.