"""Check message menu geometry and live hit testing through the native terminal."""
import re

from native_smoke import Terminal


def locate(terminal, text):
    terminal.expect(text)
    return next((row.index(text), y) for y, row in enumerate(terminal.screen.display) if text in row)


def click(terminal, x, y, button=0):
    terminal.send(f'\x1b[<{button};{x + 1};{y + 1}M', wait=.03)
    terminal.send(f'\x1b[<{button};{x + 1};{y + 1}m')


def menu(terminal, title='Message actions'):
    title_x, title_y = locate(terminal, title)
    left, top = title_x - 2, title_y - 1
    row = terminal.screen.display[top]
    assert row[left] == '╭', terminal.text
    width = row.index('╮', left) - left + 1
    bottom = next(y for y in range(top + 1, terminal.rows)
                  if terminal.screen.display[y][left] == '╰')
    assert terminal.screen.display[bottom][left + width - 1] == '╯', terminal.text
    return left, top, width, bottom - top + 1


def expect_anchor(terminal, pointer, title='Message actions'):
    left, top, width, height = menu(terminal, title)
    assert (left, top) == (min(pointer[0], terminal.cols - width),
                           min(pointer[1], terminal.rows - height)), (pointer, terminal.text)
    return left, top, width, height


def expect_keyboard(terminal):
    left, top, width, _ = menu(terminal)
    chat_left = 25 if terminal.cols >= 100 else 0
    assert (left, top) == (chat_left + (terminal.cols - chat_left - width) // 2, 4), terminal.text


terminal = Terminal(cols=56, rows=38)
try:
    terminal.picker('Studio')
    terminal.send('Draft remains')
    pointer = locate(terminal, 'The new layout')
    click(terminal, *pointer, button=2)
    left, top, width, height = expect_anchor(terminal, pointer)
    assert (left, top) == pointer, terminal.text
    # The old centered Reply row is outside the menu drawn at the pointer.
    click(terminal, (terminal.cols - width) // 2 + 2, 7)
    assert 'Message actions' not in terminal.text and 'Replying to' not in terminal.text, terminal.text

    click(terminal, *pointer, button=2)
    terminal.click_text('Reply')
    terminal.expect('The new layout is ready')
    terminal.expect('Replying to Leo')
    terminal.expect('› Draft remains')
    terminal.send(b'\x1b')

    terminal.resize(116, 36)
    pointer = (terminal.cols - 3, locate(terminal, 'Keep the details quiet')[1])
    click(terminal, *pointer, button=2)
    left, top, width, height = expect_anchor(terminal, pointer)
    assert left + width == terminal.cols and top + height == terminal.rows, terminal.text
    terminal.click_text('More message tools')
    expect_anchor(terminal, pointer, 'Message tools')
    terminal.expect('Edit message')

    terminal.send(b'\x1b')
    terminal.send(b'\r')
    expect_keyboard(terminal)
    terminal.send(b'\x1b')
    click(terminal, *pointer, button=2)
    terminal.resize(56, 32)
    expect_anchor(terminal, pointer)
    terminal.click_text('Reply')
    terminal.expect('Replying to You')
    terminal.expect('› Draft remains')
finally:
    terminal.close()

terminal = Terminal(cols=34, rows=38)
try:
    terminal.picker('Saved Messages')
    terminal.expect('Note 35:')
    terminal.send(b'\x1b[5~')
    note = next(re.search(r'Note \d+:', row).group() for row in terminal.screen.display
                if re.search(r'Note \d+:', row))
    assert note != 'Note 35:', terminal.text
    pointer = locate(terminal, note)
    click(terminal, *pointer, button=2)
    expect_anchor(terminal, pointer)
    terminal.click_text('Reply')
    terminal.expect('Replying to You · ' + note)
finally:
    terminal.close()

print('PASS right-click anchors at the terminal cell, clamps both axes and selects rendered actions after resize and scroll; keyboard menu stays centered')
