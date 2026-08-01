# ThaiVim

Text editor แบบ TUI (terminal UI) เขียนด้วย C++ + ncurses รองรับข้อความไทย (UTF-8)
คีย์บายด์แบบ vim/nvim, มี File Manager และ Terminal ในตัว, resize ได้แบบ real-time,
**syntax highlighting แบบ config เป็น JSON (เพิ่มภาษาใหม่ได้โดยไม่ต้อง compile)**,
**theme สี light / dark / custom**, เลขบรรทัดแบบ dynamic width และ **cursor line
highlight** (ไฮไลต์บรรทัดปัจจุบันเหมือน `:set cursorline` ของ vim/nvim)

```
┌────┬──────────┬─────────────────────────────┐
│    │          │                              │
│Menu│  Files   │           Editor             │
│    │          │                              │
│    │          ├─────────────────────────────┤
│    │          │      Shell / Terminal        │
│    │          │                              │
├────┴──────────┴─────────────────────────────┤
│  -- NORMAL -- | Focus: Editor | Ln 1, Col 1 | Theme: dark │  <- Status bar
└────────────────────────────────────────────────┘
```

## Build

ต้องมี `libncurses-dev` (หรือ `libncursesw-dev`) และ header เดี่ยวของ
`nlohmann/json.hpp` อยู่ในพาธ include (เช่นวางไว้ที่ `./nlohmann/json.hpp`)

```bash
sudo apt install libncurses-dev
g++ -std=c++17 thaivim.cpp -lncursesw -lpthread -o thaivim
./thaivim
```

**สำคัญ:** ต้องรันใน terminal ที่ locale เป็น UTF-8 (เช่น `LANG=th_TH.UTF-8` หรือ
`en_US.UTF-8`) ไม่งั้นตัวอักษรไทยจะแสดงเพี้ยน โปรแกรมเรียก `setlocale(LC_ALL, "")`
ให้อัตโนมัติแล้ว แต่ locale ต้องถูก generate ไว้ในเครื่องก่อน (`locale -a` เช็คได้)

**สำคัญอีกอย่าง:** โปรแกรมหา folder `syntax/` และ `themes/` จาก
`MotherPath` (คำนวณจาก path ของไฟล์ executable เอง ถอยขึ้นไป 2 ระดับ — ดูใน
`main()`) ดังนั้นให้วาง `syntax/` และ `themes/` ไว้เป็น sibling ของโฟลเดอร์ที่เก็บ
ไฟล์ executable ตามโครงสร้างนี้:

```
project-root/
├── syntax/
│   ├── config.json
│   ├── cpp.json
│   ├── python.json
│   ├── json.json
│   ├── markdown.json
│   └── bash.json
├── themes/
│   ├── dark.json
│   ├── light.json
│   └── custom.json
└── bin/               <- ตัว executable วางในนี้ (หรือโฟลเดอร์ย่อยชื่ออะไรก็ได้ระดับเดียว)
    └── thaivim
```

ถ้าไม่พบ `syntax/config.json` หรือ `themes/dark.json` โปรแกรมจะยังรันได้ปกติ
(แค่ไม่มี syntax highlighting และไม่มีสีธีม) เพราะทุกจุดโหลดไฟล์ด้วย try/catch
ป้องกันไฟล์ config เสียหรือหายแล้วโปรแกรมพัง

## แผงหน้าจอ (Panels) และการสลับโฟกัส

มี 3 พาเนลที่รับ input ได้: **Editor**, **File Manager**, **Terminal**

- กด **Tab** เพื่อวนโฟกัสตามลำดับ `Editor → File Manager → Terminal → Editor ...`
  (คล้าย `Ctrl-w w` ใน nvim ที่ใช้สลับ window)
- **ESC** ใน Terminal panel = กลับไป Editor ทันที

## Editor (โหมด vim-like)

### Normal mode (โหมดเริ่มต้น)

| ปุ่ม | ทำอะไร |
|---|---|
| `h` `j` `k` `l` หรือลูกศร | เลื่อน cursor ซ้าย/ลง/บน/ขวา |
| `0` | ไปต้นบรรทัด |
| `$` | ไปท้ายบรรทัด |
| `gg` | ไปบรรทัดแรกของไฟล์ |
| `G` | ไปบรรทัดสุดท้ายของไฟล์ |
| `i` | เข้า Insert mode ที่ตำแหน่ง cursor |
| `a` | เข้า Insert mode ถัดจาก cursor (append) |
| `A` | เข้า Insert mode ท้ายบรรทัด |
| `I` | เข้า Insert mode ต้นบรรทัด |
| `o` | เปิดบรรทัดใหม่ด้านล่าง แล้วเข้า Insert mode |
| `O` | เปิดบรรทัดใหม่ด้านบน แล้วเข้า Insert mode |
| `x` | ลบตัวอักษรใต้ cursor |
| `dd` | ลบทั้งบรรทัด (เก็บเข้า clipboard ในตัว) |
| `yy` | คัดลอกทั้งบรรทัด |
| `p` | วางบรรทัดที่ copy/delete ไว้ ด้านล่าง cursor |
| `P` | วางด้านบน cursor |
| `:` | เข้า Command mode |
| `Tab` | สลับโฟกัสไปพาเนลถัดไป |

> หมายเหตุ: ถ้ากดปุ่มเริ่มคำสั่งสองปุ่ม (`d`, `y`, `g`) แล้วตามด้วยปุ่มที่ไม่ตรงกัน
> (เช่น `d` แล้ว `j`) ปุ่มที่สองจะไม่ถูกทิ้ง แต่จะถูกประมวลผลเป็นคำสั่งปกติต่อทันที
> (เช่น cursor จะเลื่อนลงตาม `j`) — เดิมปุ่มที่สองจะหายไปเฉยๆ (ดูหัวข้อ Bug fixes)

### Insert mode

พิมพ์ได้ปกติ (รองรับอักขระไทยแบบ UTF-8 หลายไบต์), `Backspace`, `Enter` ขึ้นบรรทัดใหม่,
ลูกศรเลื่อน cursor, **ESC** กลับ Normal mode

### Command mode (`:`) — ระบบคำสั่งแบบ nvim

พิมพ์ `:` จาก Normal mode แล้วพิมพ์คำสั่ง กด Enter เพื่อรัน, ESC ยกเลิก,
กด **↑ / ↓** เพื่อย้อนดูคำสั่งที่เคยพิมพ์ (command history)

| คำสั่ง | ทำอะไร |
|---|---|
| `:w` | บันทึกไฟล์ (ไฟล์ที่เปิดอยู่) |
| `:w <path>` | บันทึกเป็นไฟล์ใหม่ที่ path ที่ระบุ (save-as) |
| `:q` | ออกจากโปรแกรม (ถ้ามีแก้ไขค้างจะเตือนก่อน) |
| `:q!` | ออกโดยไม่บันทึก (force quit) |
| `:wq` หรือ `:x` | บันทึกแล้วออก |
| `:e <path>` หรือ `:edit <path>` | เปิดไฟล์ที่ path ที่ระบุ (จะเลือก syntax highlighting ให้อัตโนมัติตามนามสกุล) |
| `:42` (ตัวเลขล้วน) | กระโดดไปบรรทัดที่ 42 |
| `:!<คำสั่ง>` | รันคำสั่ง shell ทันที แล้วสลับโฟกัสไป Terminal panel เพื่อดูผลลัพธ์ |
| `:term` หรือ `:sh` | สลับโฟกัสไป Terminal panel เฉยๆ (ไม่รันอะไร) |
| `:set number` | เปิดแสดงเลขบรรทัด (gutter ความกว้างปรับอัตโนมัติตามจำนวนบรรทัดของไฟล์) |
| `:set nonumber` | ปิดแสดงเลขบรรทัด |
| `:set cursorline` | เปิดไฮไลต์พื้นหลังบรรทัดปัจจุบัน (เหมือน `:set cursorline` ของ vim/nvim) |
| `:set nocursorline` | ปิดไฮไลต์บรรทัดปัจจุบัน |
| `:set theme dark` | เปลี่ยนเป็นธีมมืด (ค่าเริ่มต้น) |
| `:set theme light` | เปลี่ยนเป็นธีมสว่าง |
| `:set theme custom` | เปลี่ยนเป็นธีมที่ผู้ใช้กำหนดเอง (`themes/custom.json`) |
| `:help` | แสดงสรุปคำสั่งทั้งหมดที่แถบสถานะ |

ธีม, `:set number`, และ `:set cursorline` ที่เลือกล่าสุดจะถูกจำไว้ใน session
(`.thaivim_session.json`) แล้วโหลดกลับมาอัตโนมัติตอนเปิดโปรแกรมครั้งถัดไป

## เลขบรรทัด (Line numbers) และ Cursor line

### `:set number` — เลขบรรทัดแบบ dynamic width

เดิม gutter เลขบรรทัดกว้างคงที่ 4 หลัก (`"%4d "`) ซึ่งทำให้ไฟล์ที่มีมากกว่า 9,999
บรรทัดแสดงเลขบรรทัดล้นช่อง/บิดเบี้ยว ตอนนี้ความกว้าง gutter คำนวณจากจำนวนบรรทัด
ทั้งหมดของไฟล์ที่เปิดอยู่จริง (อย่างน้อย 4 หลักเท่าของเดิม แต่ขยายเป็น 5, 6, ... หลัก
โดยอัตโนมัติถ้าไฟล์ยาวกว่านั้น) นอกจากนี้เลขบรรทัดของบรรทัดที่ cursor อยู่จะถูกทำ
**ตัวหนา (bold)** เพื่อช่วยมองหาตำแหน่ง cursor ได้ไวขึ้นแม้ยังไม่ได้เปิด `cursorline`

### `:set cursorline` — ไฮไลต์บรรทัดปัจจุบัน

เปิดแล้วพื้นหลังทั้งบรรทัดที่ cursor อยู่ (รวมพื้นที่ว่างท้ายบรรทัดในหน้าต่าง Editor)
จะถูกไฮไลต์แบบ reverse-video ทำงานร่วมกับ syntax highlighting ได้ตามปกติ (สี
keyword/string/comment ฯลฯ ยังคงแสดงทับพื้นหลังที่ไฮไลต์) ปิด/เปิดสลับได้ตลอดเวลา
ด้วย `:set cursorline` / `:set nocursorline` และค่านี้จะถูกจำไว้ข้าม session
เหมือนกับ `:set number`

## File Manager

- `j` / `k` หรือลูกศร ขึ้น-ลง เลื่อนเลือกไฟล์/โฟลเดอร์
- `Enter` — ถ้าเลือกโฟลเดอร์ = เข้าไปข้างใน (`..` = ถอยกลับ 1 ระดับ),
  ถ้าเลือกไฟล์ = เปิดไฟล์นั้นเข้า Editor ทันทีและสลับโฟกัสไป Editor ให้เลย
  (จะเลือก syntax highlighting ให้อัตโนมัติตามนามสกุลไฟล์)
- `Tab` — สลับโฟกัสไปพาเนลถัดไป (Terminal)

## Terminal panel

ไม่ใช่ pty/terminal emulator เต็มรูปแบบ (รันโปรแกรม interactive อย่าง `vim`, `top`,
`htop` ข้างในไม่ได้) แต่เป็น **"command runner"**: พิมพ์คำสั่ง shell แล้ว Enter,
คำสั่งจะถูกรันจริงผ่าน `popen()` ใน background thread แยกต่างหาก (ไม่ทำให้ UI ค้าง)
ผลลัพธ์ (stdout รวม stderr) จะไหลเข้ามาแสดงแบบ real-time ในพาเนล

- คำสั่งจะรันโดยอ้างอิง **ไดเรกทอรีที่ File Manager เปิดอยู่ตอนนั้น** (ไม่ใช่ path ที่รันโปรแกรม)
  พาธของไดเรกทอรีจะถูก escape เครื่องหมาย `'` ให้อัตโนมัติก่อนประกอบเป็นคำสั่ง shell
  (กันกรณีเปิดโฟลเดอร์ที่ชื่อมี single quote อยู่ เช่น `John's Files` แล้วคำสั่ง `cd` พัง — ดูหัวข้อ Bug fixes)
- `clear` — ล้างหน้าจอ scrollback (คำสั่งพิเศษ ไม่ผ่าน shell จริง)
- `↑` / `↓` — ย้อนดูคำสั่งที่เคยรัน (history)
- `PageUp` / `PageDown` — เลื่อนดู scrollback ย้อนหลัง/กลับปัจจุบัน
- `ESC` — กลับ Editor, `Tab` — ไปพาเนลถัดไป

## Real-time resize

โปรแกรมเรียก `timeout(50)` ให้ `getch()` ไม่ block เกิน 50ms ทำให้ loop หลักวนได้เรื่อยๆ
แม้ไม่มีการกดปุ่ม แล้วใช้ 2 ทางร่วมกันเพื่อจับการ resize:

1. **`KEY_RESIZE`** — ที่ ncurses ส่งมาจาก `getch()` เองเมื่อได้รับ `SIGWINCH` (วิธีมาตรฐาน)
2. **`ioctl(TIOCGWINSZ)` แบบ poll ทุก tick** — เป็น fallback เผื่อบาง terminal/SSH client
   ไม่ส่ง `SIGWINCH`/`KEY_RESIZE` มาให้ ncurses ครบทุกครั้ง

ทั้งสองทางเรียก `HandleResize()` เหมือนกัน: `endwin()` → อ่านขนาดจอใหม่ → `delwin()`
หน้าต่างเดิมทั้งหมด → เรียก `WriteTUI()` สร้างหน้าต่างใหม่ตามสัดส่วนเดิม (Menu 1/20,
Files 1/4, Editor 3/4 ของพื้นที่ที่เหลือ, Shell 1/4) เนื้อหา (`CurBuf`, `FMEntries`,
`Term.outputLines`) เป็น global state แยกจาก window จึงไม่หายหลัง resize —
`RenderAll()` รอบถัดไปจะวาดเนื้อหาทั้งหมดกลับเข้าหน้าต่างใหม่ให้เอง (รวมถึงสี theme
ผ่าน `ApplyThemeToWindows()` ที่ถูกเรียกท้าย `WriteTUI()` ทุกครั้ง)

## Syntax Highlighting (เพิ่มภาษาใหม่ได้เองผ่าน JSON — ไม่ต้อง compile ใหม่)

Highlighting ทั้งหมดถูก config เป็นไฟล์ `.json` อยู่ใน folder `syntax/`
ไม่มี logic ของภาษาใดฝังอยู่ใน source code เลย — เพิ่ม/แก้ภาษาทำได้โดยแก้ไฟล์ JSON
เท่านั้น

### โครงสร้าง

```
syntax/
├── config.json     <- map "นามสกุลไฟล์" -> "ชื่อไฟล์ syntax definition"
├── cpp.json
├── python.json
├── json.json
├── markdown.json
└── bash.json
```

**`syntax/config.json`** กำหนดว่านามสกุลไฟล์ไหนใช้ syntax definition อันไหน:

```json
{
  "extensions": {
    ".cpp": "cpp",
    ".hpp": "cpp",
    ".py": "python",
    ".json": "json"
  }
}
```

เมื่อเปิดไฟล์ (ผ่าน `:e`, File Manager, หรือ session ที่บันทึกไว้) โปรแกรมจะดูนามสกุล
ไฟล์ แล้วโหลดสี highlight จาก `syntax/<ชื่อที่ map ไว้>.json` ให้อัตโนมัติ ถ้าไม่รู้จัก
นามสกุลนั้น ไฟล์จะเปิดแบบไม่มีสี (ธรรมดา) — ไม่ error

### รูปแบบไฟล์ syntax definition หนึ่งภาษา (เช่น `syntax/cpp.json`)

```json
{
  "name": "cpp",
  "line_comment": "//",
  "block_comment_start": "/*",
  "block_comment_end": "*/",
  "string_delimiters": ["\"", "'"],
  "preprocessor": true,
  "numbers": true,
  "keywords": ["if", "else", "for", "while", "return", "class", "..."],
  "types": ["int", "float", "string", "vector", "..."]
}
```

| key | ความหมาย |
|---|---|
| `name` | ชื่อภาษา (แสดงในแถบชื่อ Editor เช่น `Editor - main.cpp (cpp)`) |
| `line_comment` | ตัวเริ่ม comment บรรทัดเดียว เช่น `//` หรือ `#` (ใส่ `""` ถ้าภาษาไม่มี) |
| `block_comment_start` / `block_comment_end` | ตัวเริ่ม/ปิด comment หลายบรรทัด เช่น `/*` `*/` หรือ `<!--` `-->` (ใส่ `""` ถ้าไม่มี) |
| `string_delimiters` | อาร์เรย์ของอักขระที่ใช้เปิด/ปิดสตริง แต่ละตัวยาว 1 ตัวอักษร เช่น `["\"", "'"]` (รองรับ escape ด้วย `\`) |
| `preprocessor` | `true` = บรรทัดที่ขึ้นต้นด้วย `#` (หลัง trim ช่องว่าง) จะได้สี preprocessor ทั้งบรรทัด (ใช้กับ `#include`, `#define` ของ C/C++) |
| `numbers` | `true` = ไฮไลต์ตัวเลขด้วยสี number |
| `keywords` | รายการคำสงวนของภาษา (สี keyword) |
| `types` | รายการชื่อชนิดข้อมูล/คลาสมาตรฐาน (สี type, แยกจาก keyword เพื่อให้ปรับสีเองได้อิสระ) |

### เพิ่มภาษาใหม่ 2 ขั้นตอน

1. สร้างไฟล์ `syntax/<ชื่อภาษา>.json` ตามฟอร์แมตด้านบน เช่น `syntax/rust.json`
2. เพิ่ม mapping นามสกุลไฟล์ใน `syntax/config.json`:
   ```json
   "extensions": { "...": "...", ".rs": "rust" }
   ```

แค่นี้ก็เปิดไฟล์ `.rs` แล้วได้ syntax highlighting ทันที ไม่ต้อง compile ใหม่

### Performance: ทำไมเปิดไฟล์ใหญ่แล้วไม่หน่วง

Highlighting ถูก cache เป็นรายบรรทัดใน `HLCache` (ขนานกับ `CurBuf.lines`) แต่ละ entry
เก็บ hash ของเนื้อหาบรรทัด + "สถานะเข้า" (เช่น อยู่ใน `/* ... */` ที่ยังไม่ปิดจาก
บรรทัดก่อนหน้าหรือเปล่า) + spans (ผลลัพธ์สีที่คำนวณไว้แล้ว)

ทุกครั้งที่ render (`EnsureHighlightValid`) จะไล่ตั้งแต่บรรทัดแรกถึงบรรทัดสุดท้ายที่
มองเห็นบนจอ (**ไม่ใช่ทั้งไฟล์**) และสำหรับแต่ละบรรทัด:

- ถ้า hash เดิม + สถานะเข้าเดิม → **ข้ามการคำนวณ** ใช้ผลลัพธ์เดิม (O(1))
- ถ้าเปลี่ยน (เพิ่งแก้ไข หรือสถานะ block-comment จากบรรทัดก่อนเปลี่ยน) → คำนวณใหม่
  เฉพาะบรรทัดนั้น แล้ว cache ผลลัพธ์ไว้

ผลคือ: แก้ไขข้อความกลางไฟล์ใหญ่ๆ จะ re-parse แค่บรรทัดที่แก้ (บวกบรรทัดถัดไปถ้า
สถานะ comment เปลี่ยนจริง) ไม่ใช่ทั้งไฟล์ทุกครั้งที่กดปุ่ม และเลื่อนดูไฟล์ยาวๆ ก็ไม่
ต้อง parse ซ้ำของบรรทัดที่เคยเห็นแล้ว ตัวไฮไลต์ cursorline (ด้านบน) เป็นแค่ attribute
ที่ทับตอน render เท่านั้น ไม่กระทบ cache นี้เลย

## Theme: Light / Dark / Custom

สี highlighting และพื้นหลังของทุก panel ถูก config เป็น JSON อยู่ใน folder `themes/`
(แยกจาก `syntax/` ที่ใช้กำหนด "อะไรควรมีสีอะไร" — `themes/` กำหนดว่า "สีนั้นคือสีอะไร
จริงๆ")

```
themes/
├── dark.json     <- ธีมมืด (ค่าเริ่มต้น)
├── light.json    <- ธีมสว่าง
└── custom.json   <- ธีมที่ผู้ใช้แก้เองได้อิสระ (แก้ไฟล์นี้ตรงๆ ได้เลย)
```

รูปแบบไฟล์ธีม:

```json
{
  "name": "dark",
  "colors": {
    "background": "black",
    "foreground": "white",
    "keyword": "blue",
    "type": "cyan",
    "string": "green",
    "comment": "white",
    "number": "magenta",
    "preprocessor": "yellow"
  }
}
```

สีที่ใช้ได้ (เพราะ ncurses มาตรฐานรองรับ 8 สีนี้): `black`, `red`, `green`, `yellow`,
`blue`, `magenta`, `cyan`, `white`

สลับธีมได้ระหว่างใช้งานด้วยคำสั่ง:

```
:set theme dark
:set theme light
:set theme custom
```

**สร้างธีมของตัวเองเพิ่ม** ทำได้โดยสร้างไฟล์ใหม่ เช่น `themes/solarized.json`
ตามฟอร์แมตด้านบน แล้วเรียก `:set theme solarized` ได้เลย ไม่ต้อง compile ใหม่
(หรือจะแก้ `themes/custom.json` ตรงๆ แล้ว `:set theme custom` ก็ได้เช่นกัน)

ธีมที่เลือกไว้ล่าสุดจะถูกบันทึกใน `.thaivim_session.json` และโหลดกลับมาอัตโนมัติ

## สถาปัตยกรรม / ระบบ Queue ทำงานยังไง (สำหรับต่อยอด)

โปรแกรมมี 2 thread หลัก:

1. **`main()`** (thread หลักของโปรเซส) — เป็น *dispatcher loop*: วนเช็ค `MainQueue`
   (ป้องกันด้วย `QueueMutex`) ถ้ามี `QueueNode` เข้ามาก็ประมวลผลตาม `action`
   (ตอนนี้รองรับแค่ `"QUIT"` → set `MainRunning=false`) ถ้าไม่มีงานก็
   `sleep_for(20ms)` กัน busy-loop กิน CPU 100%
   → **นี่คือจุดต่อยอดหลัก**: ถ้าจะเพิ่มฟีเจอร์ async ใหม่ (เช่น auto-save เป็นระยะ,
   file-watcher, plugin system) ให้ push `QueueNode{sender, action, parameters}`
   เข้า `MainQueue` แล้วมาเพิ่ม `if (node.action == "...")` ใน loop นี้

2. **`main_thread()`** (spawn จาก `main()` แล้ว detach) — เป็น *UI thread*: เรียก
   `SetupTerminal()` สร้างหน้าต่าง ncurses ทั้งหมด (โหลด syntax defs + theme เริ่มต้น
   ด้วย) แล้ววน loop `getch()` → `HandleKey()` → `RenderAll()` ไปเรื่อยๆ จนกว่า
   `MainRunning` จะเป็น false เมื่อออกจาก loop จะ `endwin()` แล้ว push
   `QueueNode{action="QUIT"}` เข้า `MainQueue` เพื่อบอกให้ `main()` เลิก loop และจบ
   โปรแกรม

3. **Terminal command threads** — ทุกครั้งที่รันคำสั่งใน Terminal panel (`RunTerminalCommand`)
   จะ spawn `std::thread` ใหม่แบบ `detach()` ให้ไปรัน `popen()` เอง แล้วเขียนผลลัพธ์เข้า
   `Term.outputLines` (ป้องกันด้วย `TermMutex` เพราะถูกอ่าน/เขียนจากคนละ thread กับ UI)
   UI thread แค่มา render ค่าล่าสุดทุก 50ms เท่านั้น จึงดู "real-time" โดยไม่ต้องรอ
   Enter/keypress ใดๆ

Queue ที่เหลือ (`FileManagerQueue`, `TextEditorQueue`, `OtherQueue`) ยังไม่ถูกใช้งานจริง
— เตรียมไว้เป็นโครงให้ future feature เช่น background directory-scan ของ File Manager
หรือ auto-save ของ Editor ใช้รูปแบบเดียวกับ Terminal command thread ได้เลย (spawn thread
→ ใส่ mutex คู่กัน → เขียนผลกลับ global state → ให้ `RenderAll()` วาดใหม่)

## Command registry (เพิ่มคำสั่ง `:` เองได้ง่ายๆ)

คำสั่ง `:` ทั้งหมดถูกเก็บใน

```cpp
map<string, function<void(vector<string>& args, string& statusMsg)>> Commands;
```

เพิ่มคำสั่งใหม่โดยแก้ใน `RegisterCommands()`:

```cpp
Commands["ชื่อคำสั่งใหม่"] = [](vector<string>& args, string& statusMsg) {
    // args คือ token หลังชื่อคำสั่ง (แยกด้วยช่องว่าง)
    statusMsg = "ทำงานแล้ว!";
};
```

`ParseAndExecuteCommand()` จะ tokenize คำสั่งด้วยช่องว่างก่อนเข้า registry นี้
(กรณีพิเศษที่เช็คก่อนเข้าคือ `!` นำหน้า = รัน shell ทันที และตัวเลขล้วน = jump to line)

## Bug fixes (รอบนี้)

รายการบั๊ก TUI / crash ที่แก้ไปในเวอร์ชันนี้ พร้อมสาเหตุและวิธีแก้:

1. **เลขบรรทัดล้นช่องในไฟล์ยาว (>9,999 บรรทัด)** — เดิม gutter กำหนดความกว้างคงที่
   ด้วย `"%4d "` ทำให้เลขบรรทัด 5 หลักขึ้นไปแสดงผิดตำแหน่งหรือชนกับเนื้อหา แก้โดยเพิ่ม
   `DigitCount()` คำนวณความกว้าง gutter จากจำนวนบรรทัดจริงของไฟล์ (ขั้นต่ำ 4 หลักเท่าเดิม
   ขยายอัตโนมัติถ้ายาวกว่านั้น)
2. **โปรแกรม crash เมื่อกระโดดไปเลขบรรทัดที่ใหญ่เกิน `int`** — `:999999999999999`
   (ตัวเลขล้วนที่เกินช่วง `int`) เดิมเรียก `stoi()` ตรงๆ ซึ่ง throw `std::out_of_range`
   และไม่มีการดัก exception ใน UI thread (`main_thread` ไม่มี try/catch ครอบ)
   ทำให้ `std::terminate()` และโปรแกรมปิดตัวทันที ตอนนี้ครอบ `try/catch` และแสดง
   status message แทนการ crash
3. **ปุ่มที่สองหายไปเฉยๆ หลังกด `d`/`y`/`g` ค้าง (pending key) แล้วตามด้วยปุ่มที่ไม่ตรงชุด**
   — เช่นกด `d` ตามด้วย `j` (ไม่ใช่ `dd`) เดิมปุ่ม `j` จะถูกทิ้งไปเฉยๆ ไม่ขยับ cursor
   ตามที่ควรจะเป็น ตอนนี้ปุ่มที่สองที่ไม่ match pending combo จะถูกส่งเข้า
   `HandleNormalKey()` ต่อทันทีเหมือนกดปุ่มนั้นตามปกติ (ไม่มี `d`/`y`/`g` ค้างอยู่ก่อน)
4. **`cd` ใน Terminal panel พังถ้าไดเรกทอรีมีเครื่องหมาย `'` ในชื่อ** — เดิมประกอบคำสั่ง
   shell ด้วย `"cd '" + cwd.string() + "'"` ตรงๆ ถ้าชื่อโฟลเดอร์มี `'` (เช่น
   `John's Files`) จะทำให้ quote ปิดก่อนกำหนดและคำสั่งพัง เพิ่มฟังก์ชัน
   `ShellEscapeSingleQuoted()` escape เครื่องหมาย `'` ให้ถูกต้องก่อนประกอบคำสั่งเสมอ

## ข้อจำกัดที่รู้อยู่แล้ว / แนวทางต่อยอด

- **สระ/วรรณยุกต์ไทยแบบ combining mark** ยังนับความกว้างจอ = 1 ช่องเท่าตัวอักษรทั่วไป
  (ไม่ใช่ zero-width จริงแบบ terminal emulator เต็มรูปแบบ) — ถ้าจะแก้ให้แม่นขึ้นต้องทำ
  ตาราง Thai combining-character class เอง แล้วปรับ `screenX` ตอน render/cursor
- **Terminal ไม่ใช่ pty จริง** รันโปรแกรม interactive ข้างในไม่ได้ (vim, top, ssh แบบ
  ต้องพิมพ์โต้ตอบ) ถ้าต้องการ terminal เต็มรูปแบบ ต้องใช้ `forkpty()` +
  ไลบรารีแปลผล ANSI escape sequence เช่น `libvterm` แล้ว render buffer ของมันแทน
  scrollback แบบ line-by-line ตอนนี้
- **ยกเลิกคำสั่งที่กำลังรันใน Terminal** (Ctrl+C) ยังไม่รองรับ — ต้องเก็บ `pid_t`
  จาก `fork()`/`exec()` เอง (แทน `popen()`) แล้ว `kill(pid, SIGINT)` เมื่อกด Ctrl+C
- **ไม่มี Visual mode / split windows / search (`/`)** แบบ nvim จริง — เป็นจุดต่อยอดที่ทำได้
  โดยเพิ่ม `EditorMode::VISUAL` ใหม่ และเก็บ selection range ใน `EditorBuffer`
- **`:w <path>` ไม่มี tab-completion** ของชื่อไฟล์ — ถ้าจะเพิ่มต้องอ่าน `FMEntries`/
  `fs::directory_iterator` มา filter ตาม prefix ที่พิมพ์ค้างใน `CmdLineInput`
- **Syntax highlighting เป็น byte-level tokenizer ธรรมดา** ไม่ใช่ full parser/AST
  ดังนั้นกรณีซับซ้อน เช่น string ที่มี nested quote แปลกๆ, raw string literal
  (`R"(...)"` ของ C++), หรือ f-string ของ Python (`f"{x}"`) จะยังไม่ไฮไลต์ตัวแปรข้างใน
  ({}) แยกสี — ถ้าจะรองรับต้องเพิ่ม field พิเศษใน syntax json (เช่น `"raw_string_prefix"`)
  แล้วเพิ่ม case ใน `ComputeSpans()`
- **สี theme จำกัดแค่ 8 สีมาตรฐานของ ncurses** (`black/red/green/yellow/blue/magenta/cyan/white`)
  เพราะไม่ได้เรียก `init_extended_color`/256-color — ถ้าต้องการสี custom แบบ RGB/256-color
  ต้องเช็ค `can_change_color()` แล้วใช้ `init_extended_color()` แทน `init_pair` ธรรมดา
- **cursorline ยังไม่มีสีพื้นหลังแบบ config ได้จาก theme** — ตอนนี้ใช้ `A_REVERSE`
  (สลับสี fg/bg) ตรงๆ ยังไม่มี key `cursorline_bg` ใน `themes/*.json` ถ้าต้องการ
  ปรับสีเฉพาะให้เพิ่ม key ใหม่ใน theme แล้วแก้ `RenderTextEditor()` ให้ใช้
  `init_pair`/`COLOR_PAIR` แทน `A_REVERSE`