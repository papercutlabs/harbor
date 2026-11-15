# Harbor 0.1.14

Left and right arrows are now explicitly decoded for CSI, application SS3, and CSI-u terminal input. Previously, normal CSI arrows could insert literal [D or [C into the draft when the terminal definition expected application arrows. A native PTY regression reproduced the corruption before the fix and verifies cursor movement for all supported variants after it.
