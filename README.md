# m5examples

M5StickC examples.

This repository is a collection of independent [PlatformIO](https://platformio.org/)
projects. Each top-level subdirectory is its own self-contained project with its
own `platformio.ini`, `src/`, `lib/`, `include/`, and `test/` folders.

## Layout

```
m5examples/
├── .gitignore        # repo-wide ignores (covers every project's build artifacts)
├── LICENSE
├── README.md
├── geodash/          # a PlatformIO project (Geometry Dash mini-game)
│   ├── platformio.ini
│   ├── src/
│   ├── include/
│   ├── lib/
│   └── test/
└── magic-8-ball/     # voice-prompted Magic 8-Ball (uses M5Unified for the mic)
    ├── platformio.ini
    └── src/
```

## Building a project

Open the individual project directory (not the repository root) in your editor,
or build from the command line:

```sh
cd geodash
pio run                 # build
pio run -t upload       # build and flash
pio device monitor      # serial monitor
```

Each project targets the M5StickC Plus (`board = m5stick-c`,
`framework = arduino`).

## Adding a new project

Create a new subdirectory and initialize it as a PlatformIO project:

```sh
mkdir my-example
cd my-example
pio project init --board m5stick-c
```
