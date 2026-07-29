"""Build the SyncCinema engineering journal DOCX.

The DOCX is committed together with this source file:
- the Python source keeps document changes reviewable in Git;
- the DOCX gives the project owner a convenient learning/interview artifact.
"""

from pathlib import Path

from docx import Document
from docx.enum.section import WD_SECTION
from docx.enum.table import WD_CELL_VERTICAL_ALIGNMENT, WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_BREAK, WD_LINE_SPACING
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Inches, Pt, RGBColor


OUTPUT_PATH = Path(__file__).with_name("SyncCinema_Engineering_Journal.docx")

FONT_BODY = "Microsoft YaHei"
FONT_CODE = "Consolas"
COLOR_BLUE = RGBColor(46, 116, 181)
COLOR_DARK_BLUE = RGBColor(31, 77, 120)
COLOR_INK = RGBColor(30, 38, 48)
COLOR_MUTED = RGBColor(91, 101, 113)
COLOR_LIGHT_BLUE = "E8EEF5"
COLOR_LIGHT_GRAY = "F2F4F7"
COLOR_CALLOUT = "F4F6F9"
COLOR_BORDER = "C8D2DE"

PAGE_WIDTH_DXA = 9360
TABLE_INDENT_DXA = 120


def set_run_font(
    run,
    name=FONT_BODY,
    size=10.5,
    color=COLOR_INK,
    bold=None,
    italic=None,
):
    run.font.name = name
    run._element.get_or_add_rPr().rFonts.set(qn("w:ascii"), name)
    run._element.get_or_add_rPr().rFonts.set(qn("w:hAnsi"), name)
    run._element.get_or_add_rPr().rFonts.set(qn("w:eastAsia"), name)
    run.font.size = Pt(size)
    run.font.color.rgb = color
    if bold is not None:
        run.bold = bold
    if italic is not None:
        run.italic = italic


def set_paragraph_spacing(paragraph, before=0, after=6, line_spacing=1.25):
    fmt = paragraph.paragraph_format
    fmt.space_before = Pt(before)
    fmt.space_after = Pt(after)
    fmt.line_spacing = line_spacing


def set_cell_shading(cell, fill):
    tc_pr = cell._tc.get_or_add_tcPr()
    shading = tc_pr.find(qn("w:shd"))
    if shading is None:
        shading = OxmlElement("w:shd")
        tc_pr.append(shading)
    shading.set(qn("w:fill"), fill)


def set_cell_margins(cell, top=80, start=120, bottom=80, end=120):
    tc = cell._tc
    tc_pr = tc.get_or_add_tcPr()
    tc_mar = tc_pr.first_child_found_in("w:tcMar")
    if tc_mar is None:
        tc_mar = OxmlElement("w:tcMar")
        tc_pr.append(tc_mar)

    for margin_name, value in (
        ("top", top),
        ("start", start),
        ("bottom", bottom),
        ("end", end),
    ):
        node = tc_mar.find(qn(f"w:{margin_name}"))
        if node is None:
            node = OxmlElement(f"w:{margin_name}")
            tc_mar.append(node)
        node.set(qn("w:w"), str(value))
        node.set(qn("w:type"), "dxa")


def set_table_borders(table, color=COLOR_BORDER, size=4):
    tbl_pr = table._tbl.tblPr
    borders = tbl_pr.find(qn("w:tblBorders"))
    if borders is None:
        borders = OxmlElement("w:tblBorders")
        tbl_pr.append(borders)

    for edge in ("top", "left", "bottom", "right", "insideH", "insideV"):
        border = borders.find(qn(f"w:{edge}"))
        if border is None:
            border = OxmlElement(f"w:{edge}")
            borders.append(border)
        border.set(qn("w:val"), "single")
        border.set(qn("w:sz"), str(size))
        border.set(qn("w:space"), "0")
        border.set(qn("w:color"), color)


def set_table_geometry(table, column_widths_dxa):
    """Apply fixed OOXML geometry so Word/LibreOffice render identically."""
    if sum(column_widths_dxa) != PAGE_WIDTH_DXA:
        raise ValueError("Table column widths must total 9360 DXA")

    table.alignment = WD_TABLE_ALIGNMENT.LEFT
    table.autofit = False
    tbl = table._tbl
    tbl_pr = tbl.tblPr

    tbl_w = tbl_pr.find(qn("w:tblW"))
    if tbl_w is None:
        tbl_w = OxmlElement("w:tblW")
        tbl_pr.append(tbl_w)
    tbl_w.set(qn("w:w"), str(PAGE_WIDTH_DXA))
    tbl_w.set(qn("w:type"), "dxa")

    tbl_ind = tbl_pr.find(qn("w:tblInd"))
    if tbl_ind is None:
        tbl_ind = OxmlElement("w:tblInd")
        tbl_pr.append(tbl_ind)
    tbl_ind.set(qn("w:w"), str(TABLE_INDENT_DXA))
    tbl_ind.set(qn("w:type"), "dxa")

    layout = tbl_pr.find(qn("w:tblLayout"))
    if layout is None:
        layout = OxmlElement("w:tblLayout")
        tbl_pr.append(layout)
    layout.set(qn("w:type"), "fixed")

    grid = tbl.tblGrid
    for child in list(grid):
        grid.remove(child)
    for width in column_widths_dxa:
        grid_col = OxmlElement("w:gridCol")
        grid_col.set(qn("w:w"), str(width))
        grid.append(grid_col)

    for row in table.rows:
        for index, cell in enumerate(row.cells):
            width = column_widths_dxa[index]
            tc_pr = cell._tc.get_or_add_tcPr()
            tc_w = tc_pr.find(qn("w:tcW"))
            if tc_w is None:
                tc_w = OxmlElement("w:tcW")
                tc_pr.append(tc_w)
            tc_w.set(qn("w:w"), str(width))
            tc_w.set(qn("w:type"), "dxa")
            cell.vertical_alignment = WD_CELL_VERTICAL_ALIGNMENT.CENTER
            set_cell_margins(cell)

    set_table_borders(table)


def mark_repeat_table_header(row):
    tr_pr = row._tr.get_or_add_trPr()
    tbl_header = OxmlElement("w:tblHeader")
    tbl_header.set(qn("w:val"), "true")
    tr_pr.append(tbl_header)


def configure_document(doc):
    section = doc.sections[0]
    section.page_width = Inches(8.5)
    section.page_height = Inches(11)
    section.top_margin = Inches(1)
    section.right_margin = Inches(1)
    section.bottom_margin = Inches(1)
    section.left_margin = Inches(1)
    section.header_distance = Inches(0.492)
    section.footer_distance = Inches(0.492)

    styles = doc.styles

    normal = styles["Normal"]
    normal.font.name = FONT_BODY
    normal._element.rPr.rFonts.set(qn("w:ascii"), FONT_BODY)
    normal._element.rPr.rFonts.set(qn("w:hAnsi"), FONT_BODY)
    normal._element.rPr.rFonts.set(qn("w:eastAsia"), FONT_BODY)
    normal.font.size = Pt(10.5)
    normal.font.color.rgb = COLOR_INK
    normal.paragraph_format.space_before = Pt(0)
    normal.paragraph_format.space_after = Pt(6)
    normal.paragraph_format.line_spacing = 1.25

    heading_tokens = {
        "Heading 1": (16, COLOR_BLUE, 18, 10),
        "Heading 2": (13, COLOR_BLUE, 14, 7),
        "Heading 3": (12, COLOR_DARK_BLUE, 10, 5),
    }
    for style_name, (size, color, before, after) in heading_tokens.items():
        style = styles[style_name]
        style.font.name = FONT_BODY
        style._element.rPr.rFonts.set(qn("w:ascii"), FONT_BODY)
        style._element.rPr.rFonts.set(qn("w:hAnsi"), FONT_BODY)
        style._element.rPr.rFonts.set(qn("w:eastAsia"), FONT_BODY)
        style.font.size = Pt(size)
        style.font.bold = True
        style.font.color.rgb = color
        style.paragraph_format.space_before = Pt(before)
        style.paragraph_format.space_after = Pt(after)
        style.paragraph_format.keep_with_next = True

    for list_style_name in ("List Bullet", "List Number"):
        style = styles[list_style_name]
        style.font.name = FONT_BODY
        style._element.rPr.rFonts.set(qn("w:eastAsia"), FONT_BODY)
        style.font.size = Pt(10.5)
        style.paragraph_format.left_indent = Inches(0.375)
        style.paragraph_format.first_line_indent = Inches(-0.188)
        style.paragraph_format.space_after = Pt(4)
        style.paragraph_format.line_spacing = 1.25

    header = section.header
    header_paragraph = header.paragraphs[0]
    header_paragraph.alignment = WD_ALIGN_PARAGRAPH.LEFT
    set_paragraph_spacing(header_paragraph, after=0, line_spacing=1.0)
    run = header_paragraph.add_run("SyncCinema  |  工程演进与面试复盘")
    set_run_font(run, size=8.5, color=COLOR_MUTED, bold=True)

    footer = section.footer
    footer_paragraph = footer.paragraphs[0]
    footer_paragraph.alignment = WD_ALIGN_PARAGRAPH.RIGHT
    set_paragraph_spacing(footer_paragraph, after=0, line_spacing=1.0)
    run = footer_paragraph.add_run("第 ")
    set_run_font(run, size=8.5, color=COLOR_MUTED)
    field_begin = OxmlElement("w:fldChar")
    field_begin.set(qn("w:fldCharType"), "begin")
    instruction = OxmlElement("w:instrText")
    instruction.set(qn("xml:space"), "preserve")
    instruction.text = " PAGE "
    field_end = OxmlElement("w:fldChar")
    field_end.set(qn("w:fldCharType"), "end")
    run._r.append(field_begin)
    run._r.append(instruction)
    run._r.append(field_end)
    run = footer_paragraph.add_run(" 页")
    set_run_font(run, size=8.5, color=COLOR_MUTED)


def add_title_block(doc):
    spacer = doc.add_paragraph()
    set_paragraph_spacing(spacer, after=12)

    kicker = doc.add_paragraph()
    set_paragraph_spacing(kicker, after=4, line_spacing=1.0)
    run = kicker.add_run("ENGINEERING JOURNAL")
    set_run_font(run, size=9.5, color=COLOR_BLUE, bold=True)

    title = doc.add_paragraph()
    set_paragraph_spacing(title, after=6, line_spacing=1.0)
    run = title.add_run("SyncCinema 工程演进与面试复盘")
    set_run_font(run, size=25, color=COLOR_DARK_BLUE, bold=True)

    subtitle = doc.add_paragraph()
    set_paragraph_spacing(subtitle, after=18, line_spacing=1.15)
    run = subtitle.add_run("从命令行同步观影 MVP 到可测量、可校正、可产品化的多人观影系统")
    set_run_font(run, size=12.5, color=COLOR_MUTED)

    metadata = [
        ("当前开发分支", "feature/sync-metrics"),
        ("当前已发布提交", "2c1bf4f - pairwise playback skew analysis"),
        ("本次阶段", "公网指标验证、控制 epoch 与稳健偏差窗口"),
        ("维护日期", "2026-07-28"),
        ("安全约束", "服务器口令、私钥和其他凭据不得进入源码、日志、文档或 Git 历史"),
    ]
    table = doc.add_table(rows=1, cols=2)
    set_table_geometry(table, [2700, 6660])
    for index, header in enumerate(("项目元数据", "当前值")):
        cell = table.rows[0].cells[index]
        set_cell_shading(cell, COLOR_LIGHT_BLUE)
        paragraph = cell.paragraphs[0]
        paragraph.alignment = WD_ALIGN_PARAGRAPH.CENTER
        set_paragraph_spacing(paragraph, after=0, line_spacing=1.15)
        run = paragraph.add_run(header)
        set_run_font(run, size=9, bold=True, color=COLOR_DARK_BLUE)
    mark_repeat_table_header(table.rows[0])

    for label, value in metadata:
        label_cell, value_cell = table.add_row().cells
        set_cell_shading(label_cell, COLOR_LIGHT_BLUE)
        for cell, text, bold in (
            (label_cell, label, True),
            (value_cell, value, False),
        ):
            paragraph = cell.paragraphs[0]
            set_paragraph_spacing(paragraph, after=0, line_spacing=1.15)
            run = paragraph.add_run(text)
            set_run_font(run, size=9.5, bold=bold)

    note = doc.add_paragraph()
    set_paragraph_spacing(note, before=14, after=4, line_spacing=1.25)
    run = note.add_run("文档定位：")
    set_run_font(run, bold=True, color=COLOR_DARK_BLUE)
    run = note.add_run(
        "每个工程阶段都记录问题背景、设计判断、核心代码、验证证据、已知局限和面试表达。"
        "后续即使暂时不参与编码，也能据此重新进入项目。"
    )
    set_run_font(run)


def add_body(doc, text, bold_prefix=None):
    paragraph = doc.add_paragraph()
    set_paragraph_spacing(paragraph)
    if bold_prefix and text.startswith(bold_prefix):
        prefix_run = paragraph.add_run(bold_prefix)
        set_run_font(prefix_run, bold=True, color=COLOR_DARK_BLUE)
        text = text[len(bold_prefix):]
    run = paragraph.add_run(text)
    set_run_font(run)
    return paragraph


def add_bullet(doc, text):
    paragraph = doc.add_paragraph(style="List Bullet")
    run = paragraph.add_run(text)
    set_run_font(run)
    return paragraph


def add_numbered(doc, text):
    paragraph = doc.add_paragraph(style="List Number")
    run = paragraph.add_run(text)
    set_run_font(run)
    return paragraph


def add_callout(doc, label, text):
    paragraph = doc.add_paragraph()
    set_paragraph_spacing(paragraph, before=6, after=8, line_spacing=1.25)
    p_pr = paragraph._p.get_or_add_pPr()
    shading = OxmlElement("w:shd")
    shading.set(qn("w:fill"), COLOR_CALLOUT)
    p_pr.append(shading)
    ind = OxmlElement("w:ind")
    ind.set(qn("w:left"), "160")
    ind.set(qn("w:right"), "160")
    p_pr.append(ind)
    run = paragraph.add_run(f"{label}：")
    set_run_font(run, bold=True, color=COLOR_DARK_BLUE)
    run = paragraph.add_run(text)
    set_run_font(run)
    return paragraph


def add_code_block(doc, lines):
    paragraph = doc.add_paragraph()
    set_paragraph_spacing(paragraph, before=4, after=8, line_spacing=1.1)
    p_pr = paragraph._p.get_or_add_pPr()
    shading = OxmlElement("w:shd")
    shading.set(qn("w:fill"), "F7F8FA")
    p_pr.append(shading)
    ind = OxmlElement("w:ind")
    ind.set(qn("w:left"), "180")
    ind.set(qn("w:right"), "180")
    p_pr.append(ind)
    for index, line in enumerate(lines):
        run = paragraph.add_run(line)
        set_run_font(run, name=FONT_CODE, size=8.5, color=COLOR_INK)
        if index < len(lines) - 1:
            run.add_break()
    return paragraph


def add_commit_history_table(doc):
    rows = [
        ("5b76dc3", "Initial SyncCinema libVLC MVP", "真实本地视频可被 play/pause/seek 控制"),
        ("991a4f4", "multi-client broadcast architecture", "server 协调多个 client 并广播控制命令"),
        ("3483a28", "cross-platform Linux server build", "Linux 云端可单独构建轻量 server"),
        ("d0d91e3", "stabilize cross-platform builds", "统一源码编码与 Windows/Linux 构建行为"),
        ("90e5e3e", "HTTP media source", "client 可播放 Nginx 提供的云端视频"),
        ("ab2adc8", "client startup timings", "量化播放器初始化、媒体打开和 TCP 连接耗时"),
        ("c7caec9", "progress drift analysis", "上报真实播放器进度并比较 Room 理论进度"),
        ("02f6aba", "heartbeat RTT measurements", "以 PING/PONG 可靠测量每个 client 的 RTT"),
        ("2c1bf4f", "pairwise playback skew analysis", "把两端上报投影到同一时刻，直接测量客户端间偏差"),
    ]

    table = doc.add_table(rows=1, cols=3)
    set_table_geometry(table, [1450, 3300, 4610])
    headers = ("提交", "阶段", "可验证结果")
    for index, header in enumerate(headers):
        cell = table.rows[0].cells[index]
        set_cell_shading(cell, COLOR_LIGHT_BLUE)
        paragraph = cell.paragraphs[0]
        paragraph.alignment = WD_ALIGN_PARAGRAPH.CENTER
        set_paragraph_spacing(paragraph, after=0, line_spacing=1.15)
        run = paragraph.add_run(header)
        set_run_font(run, size=9, bold=True, color=COLOR_DARK_BLUE)
    mark_repeat_table_header(table.rows[0])

    for commit, stage, result in rows:
        cells = table.add_row().cells
        for index, text in enumerate((commit, stage, result)):
            paragraph = cells[index].paragraphs[0]
            paragraph.alignment = (
                WD_ALIGN_PARAGRAPH.CENTER if index == 0 else WD_ALIGN_PARAGRAPH.LEFT
            )
            set_paragraph_spacing(paragraph, after=0, line_spacing=1.15)
            run = paragraph.add_run(text)
            set_run_font(
                run,
                name=FONT_CODE if index == 0 else FONT_BODY,
                size=8.7 if index == 0 else 9,
            )


def add_changed_files_table(doc):
    rows = [
        (
            "SyncMetrics.h",
            "为每个 client 保存最近一次位置、状态和 server 接收时刻。",
            "让下一份上报到达时能找到可比较的另一端快照。",
        ),
        (
            "SyncMetrics.cpp",
            "新增统一时刻投影、陈旧样本过滤、状态一致性检查和 pair_progress 日志。",
            "直接得到 client A 与 client B 的相对播放偏差。",
        ),
        (
            "build_engineering_journal.py",
            "用可审查源码生成本 DOCX，固定版式和内容结构。",
            "后续每次提交都能持续维护学习与面试材料。",
        ),
    ]

    table = doc.add_table(rows=1, cols=3)
    set_table_geometry(table, [2200, 3580, 3580])
    headers = ("文件", "核心修改", "工程价值")
    for index, header in enumerate(headers):
        cell = table.rows[0].cells[index]
        set_cell_shading(cell, COLOR_LIGHT_BLUE)
        paragraph = cell.paragraphs[0]
        paragraph.alignment = WD_ALIGN_PARAGRAPH.CENTER
        set_paragraph_spacing(paragraph, after=0, line_spacing=1.15)
        run = paragraph.add_run(header)
        set_run_font(run, size=9, bold=True, color=COLOR_DARK_BLUE)
    mark_repeat_table_header(table.rows[0])

    for file_name, change, value in rows:
        cells = table.add_row().cells
        for index, text in enumerate((file_name, change, value)):
            paragraph = cells[index].paragraphs[0]
            set_paragraph_spacing(paragraph, after=0, line_spacing=1.15)
            run = paragraph.add_run(text)
            set_run_font(
                run,
                name=FONT_CODE if index == 0 else FONT_BODY,
                size=8.6 if index == 0 else 9,
            )


def add_epoch_changed_files_table(doc):
    rows = [
        (
            "SyncServer.cpp",
            "新增 controlCommandMutex，使控制命令、Room 更新、广播、epoch 切换和 REPORT 快照严格有序。",
            "避免并发 client 产生“新状态配旧统计周期”的竞态。",
        ),
        (
            "SyncMetrics.h",
            "新增 PairWindowStats、控制 epoch 状态及 beginControlEpoch()。",
            "把网络 RTT 与播放控制周期分开管理。",
        ),
        (
            "SyncMetrics.cpp",
            "控制后保留 RTT、重置偏差；加入 2 秒稳定期、12 样本窗口、中位数、P95 和连续严重偏差计数。",
            "让后续纠偏依据稳定趋势，而不是单次量化抖动。",
        ),
        (
            "Engineering Journal",
            "补录公网验证证据、算法选择、回归结果和面试表达。",
            "保证代码推进与知识交接同步完成。",
        ),
    ]

    table = doc.add_table(rows=1, cols=3)
    set_table_geometry(table, [2200, 3580, 3580])
    headers = ("文件/模块", "核心修改", "工程价值")
    for index, header in enumerate(headers):
        cell = table.rows[0].cells[index]
        set_cell_shading(cell, COLOR_LIGHT_BLUE)
        paragraph = cell.paragraphs[0]
        paragraph.alignment = WD_ALIGN_PARAGRAPH.CENTER
        set_paragraph_spacing(paragraph, after=0, line_spacing=1.15)
        run = paragraph.add_run(header)
        set_run_font(run, size=9, bold=True, color=COLOR_DARK_BLUE)
    mark_repeat_table_header(table.rows[0])

    for module, change, value in rows:
        cells = table.add_row().cells
        for index, text in enumerate((module, change, value)):
            paragraph = cells[index].paragraphs[0]
            set_paragraph_spacing(paragraph, after=0, line_spacing=1.15)
            run = paragraph.add_run(text)
            set_run_font(
                run,
                name=FONT_CODE if index == 0 else FONT_BODY,
                size=8.6 if index == 0 else 9,
            )


def build_document():
    doc = Document()
    doc.core_properties.title = "SyncCinema 工程演进与面试复盘"
    doc.core_properties.subject = "SyncCinema engineering journal"
    doc.core_properties.author = "SyncCinema Project"
    doc.core_properties.last_modified_by = "SyncCinema Project"
    doc.core_properties.keywords = "C++, TCP, libVLC, synchronization, metrics"
    configure_document(doc)
    add_title_block(doc)

    doc.add_heading("1. 当前项目全景", level=1)
    add_body(
        doc,
        "SyncCinema 当前已经不是最初的单连接 TCP demo，而是一个可以在公网运行的多人同步观影原型。"
        "视频内容通过 HTTP 分发，TCP server 只传输体积很小的控制与测量消息。"
    )
    add_callout(
        doc,
        "当前架构",
        "Windows client A/B 使用 libVLC 播放同一媒体 URL；Linux 云服务器上的 "
        "SyncCinemaServer 监听 9000 端口并维护 Room；Nginx 负责 80/443 端口的静态视频分发。",
    )
    add_code_block(
        doc,
        [
            "Windows Client A --\\",
            "                   +-- TCP :9000 --> Linux SyncCinemaServer / Room",
            "Windows Client B --/                         |",
            "                                                +-- metrics / broadcast",
            "",
            "Windows Client A/B ---- HTTP :80/443 ----> Nginx video files",
        ],
    )
    add_body(
        doc,
        "职责边界：Client 负责真实播放器和用户交互；Room 负责权威播放状态；"
        "SyncMetricsCollector 负责观测事实；Protocol 负责结构化消息与文本协议；"
        "Nginx 负责媒体内容，而不是 C++ server。",
    )

    doc.add_heading("2. 已完成的工程里程碑", level=1)
    add_body(
        doc,
        "下面的提交形成一条可解释的项目演进链。每一步都解决一个独立问题，"
        "因此既便于回滚，也方便面试时说明设计为什么逐步变化。"
    )
    add_commit_history_table(doc)

    doc.add_heading("3. 本次阶段：客户端间相对播放偏差", level=1)
    doc.add_heading("3.1 为什么原指标还不够", level=2)
    add_body(
        doc,
        "此前的 compensated_diff_ms 比较“某个 client 的 libVLC 进度”和“Room 的理论进度”。"
        "Room 在收到 PLAY 后立即按 steady_clock 前进，而 libVLC 可能先经历网络缓冲、解码和渲染准备。"
        "如果两台播放器都比 Room 晚 400 ms，但彼此只差 20 ms，旧指标仍会把两台都标成 watch。"
    )
    add_callout(
        doc,
        "核心判断",
        "用户真正感知的是两位观众看到的画面是否一致。因此自动同步算法的主要输入必须是 "
        "client-to-client 相对偏差，而 client-to-Room 偏差更适合诊断整体启动或播放器延迟。",
    )

    doc.add_heading("3.2 时间归一化算法", level=2)
    add_body(
        doc,
        "两台 client 每秒上报一次，消息不会同时到达 server。不能直接拿两条原始 position 相减，"
        "否则较早到达的样本天然更小。server 以自己的 steady_clock 为统一时间轴，"
        "把两份样本投影到同一个比较时刻。"
    )
    add_code_block(
        doc,
        [
            "Playing:",
            "projected_position = reported_position",
            "                   + estimated_one_way_delay",
            "                   + (compare_time - report_received_time)",
            "",
            "Paused / Stopped:",
            "projected_position = reported_position",
            "",
            "pair_diff_ms = projected_client_a - projected_client_b",
        ],
    )
    add_body(
        doc,
        "estimated_one_way_delay 暂时采用 min_rtt / 2。最小 RTT 更接近链路的基础传播延迟，"
        "比包含瞬时排队抖动的平均 RTT 更适合这一版估算。该假设不是最终网络时钟同步方案，"
        "但足以支撑 MVP 级相对进度测量。"
    )

    doc.add_heading("3.3 防止错误样本污染结论", level=2)
    add_bullet(doc, "超过 2500 ms 未更新的 REPORT 被视为陈旧样本，不参与当前成对比较。")
    add_bullet(doc, "两端 PlaybackState 不一致时跳过比较，因为 Playing 与 Paused 的位置推进规律不同。")
    add_bullet(doc, "client_a/client_b 始终按 id 排序，pair_diff_ms 的正负含义不会随触发方变化。")
    add_bullet(doc, "数据锁 mutex_ 与日志锁 logMutex_ 分离；计算后再输出，避免长时间占用共享状态锁。")
    add_bullet(doc, "本阶段只记录日志，不执行 seek 或倍速调整，避免未经验证的指标干扰真实播放。")

    doc.add_heading("3.4 核心修改", level=2)
    add_changed_files_table(doc)

    doc.add_heading("3.5 新日志怎么读", level=2)
    add_code_block(
        doc,
        [
            "[metric] type=pair_progress trigger_client=2 client_a=1 client_b=2",
            "state=Playing projected_a_ms=10406 projected_b_ms=10200",
            "pair_diff_ms=206 pair_abs_diff_ms=206",
            "report_age_a_ms=406 report_age_b_ms=0",
            "one_way_a_ms=0 one_way_b_ms=0 quality=good",
        ],
    )
    add_bullet(doc, "trigger_client：哪一个 client 的新 REPORT 触发了本次比较。")
    add_bullet(doc, "projected_a_ms / projected_b_ms：已经归一化到同一 server 时刻的位置。")
    add_bullet(doc, "pair_diff_ms > 0：client_a 领先；< 0：client_a 落后。")
    add_bullet(doc, "report_age_*：比较时该样本已经存在多久，用于判断样本新鲜度。")
    add_bullet(doc, "quality：当前沿用 good <= 250 ms、watch <= 1000 ms、drift > 1000 ms。")

    doc.add_heading("3.6 本地验证证据", level=2)
    add_numbered(doc, "Windows x64 Debug with libVLC 完整构建通过，SyncCinema.exe 与 SyncCinemaServer.exe 均成功链接。")
    add_numbered(doc, "WSL Ubuntu 22.04 Release server-only 构建通过，验证新增代码没有破坏 Linux 部署目标。")
    add_numbered(doc, "本地启动 server，并用两个独立 TCP client 发送错开 200 ms 的 REPORT。")
    add_numbered(doc, "第一次刻意构造约 206 ms 差距；下一轮经时间投影后得到 -3 ms，符合输入时序。")
    add_callout(
        doc,
        "验收结果",
        "新增指标能够回答“两台 client 当前相差多少毫秒”，且没有修改协议、控制链路或播放器行为。",
    )

    doc.add_heading("3.7 公网真实播放器验证", level=2)
    add_body(
        doc,
        "提交 2c1bf4f 部署到 Linux 云服务器后，使用两份真实 Windows libVLC client 播放同一个 HTTP 视频。"
        "这次验证不是只看“命令有没有广播”，而是同时观察 RTT、client-to-Room 和 pair_progress 三组指标。"
    )
    add_numbered(doc, "两台 client 到云服务器的 RTT 稳定在约 23-28 ms，说明控制链路本身没有明显拥塞。")
    add_numbered(doc, "暂停后两端真实播放器位置分别约为 74651 ms 与 74653 ms，pair_diff 约为 -2 ms。")
    add_numbered(doc, "同一时刻 client-to-Room 仍可达到约 -939 ms，证明 Room 理论时钟的落后量不能直接代表观众间偏差。")
    add_numbered(doc, "播放过程中 pair_diff 常呈现约 250 ms 的台阶，暴露出 libVLC 时间读数粒度、异步播放和每秒上报采样的共同影响。")
    add_callout(
        doc,
        "数据结论",
        "端到端同步观感已经较好，但单条 pair_diff 仍会被采样相位和播放器读数抖动影响。"
        "因此下一阶段不应立即按单个样本 seek，而应先建立控制周期和稳健统计窗口。",
    )

    doc.add_heading("3.8 仍未解决的边界", level=2)
    add_bullet(doc, "新加入房间的 client 还不会自动获取当前房间状态和进度，可能出现数十秒级真实偏差。")
    add_bullet(doc, "PLAY/SEEK 后历史统计尚未按播放 epoch 重置，旧异常值会污染长期平均值。")
    add_bullet(doc, "libVLC 的 play() 是异步请求；“API 返回”不等于首帧已经播放。")
    add_bullet(doc, "min_rtt / 2 假设上下行大致对称；移动网络或拥塞场景下可能不成立。")
    add_bullet(doc, "当前尚未实现自动纠偏、校正冷却、迟滞区间和倍速微调。")

    doc.add_heading("4. 本地阶段：控制 epoch 与稳健偏差窗口", level=1)
    doc.add_heading("4.1 为什么必须划分控制周期", level=2)
    add_body(
        doc,
        "PLAY、PAUSE、SEEK 会改变播放状态或基准位置。若 SEEK 前后的样本继续累计在同一组平均值中，"
        "旧周期的最大值、平均值和最近窗口都会污染新周期，后续自动纠偏就可能依据过期事实做决定。"
        "因此每条有效控制命令都会开启新的 playback epoch。"
    )
    add_code_block(
        doc,
        [
            "PLAY / PAUSE / SEEK accepted",
            "  -> Room applies command and broadcasts it",
            "  -> metrics.beginControlEpoch(command)",
            "  -> epoch += 1",
            "  -> clear progress and pair-window statistics",
            "  -> keep RTT statistics",
        ],
    )
    add_bullet(doc, "RTT 描述网络链路，不会因为一次 seek 自动失效，因此跨 epoch 保留。")
    add_bullet(doc, "进度偏差描述某次控制后的播放结果，必须在新 epoch 中重新采样。")
    add_bullet(doc, "控制命令与 REPORT 共用 controlCommandMutex，避免读到“新 Room 状态 + 旧 epoch”这样的撕裂快照。")

    doc.add_heading("4.2 为什么先等待 2 秒", level=2)
    add_body(
        doc,
        "libVLC 的控制 API 是异步的。命令刚返回时，播放器可能仍在缓冲、解码或寻找关键帧。"
        "新 epoch 的前 2 秒被标记为 settling：日志仍然保留，便于诊断控制响应，"
        "但这些过渡样本不进入长期偏差统计和 pair 窗口。"
    )
    add_callout(
        doc,
        "设计取舍",
        "2 秒是当前 MVP 的可配置经验值，而不是普适常量。后续应根据本地文件、HTTP 视频、"
        "不同编码格式和真实设备数据调整，或改为由播放器事件驱动稳定期结束。",
    )

    doc.add_heading("4.3 为什么选择中位数和 P95", level=2)
    add_body(
        doc,
        "稳定期结束后，每对 client 保存最近 12 个相对偏差。至少积累 6 个样本后窗口才标记为 ready。"
        "单次 250/500 ms 台阶可能只是采样相位抖动，中位数对这种离群值不敏感；"
        "P95 则保留尾部风险，用来观察绝大多数样本之外是否仍有明显卡顿。"
    )
    add_code_block(
        doc,
        [
            "window_size = 12",
            "window_ready = samples >= 6",
            "stable_skew = median(pair_diff_ms)",
            "tail_risk = P95(abs(pair_diff_ms))",
            "severe_streak += 1 only when abs(diff) > 750 ms",
        ],
    )
    add_bullet(doc, "median_diff_ms 保留正负方向：正数表示 client_a 领先，负数表示落后。")
    add_bullet(doc, "median_abs_diff_ms 表示典型偏差大小，可作为未来纠偏阈值的主要输入。")
    add_bullet(doc, "p95_abs_diff_ms 用于识别偶发严重抖动，不能单独决定立即 seek。")
    add_bullet(doc, "consecutive_severe 用于要求异常连续出现，防止一个坏样本触发控制振荡。")

    doc.add_heading("4.4 核心修改", level=2)
    add_epoch_changed_files_table(doc)

    doc.add_heading("4.5 日志字段怎么读", level=2)
    add_code_block(
        doc,
        [
            "[metric] type=control_epoch epoch=2 command=SEEK settle_ms=2000",
            "[metric] type=pair_progress client_a=1 client_b=2 epoch=2",
            "settling=0 window_samples=6 window_ready=1",
            "median_diff_ms=61 median_abs_diff_ms=61 p95_abs_diff_ms=87",
            "consecutive_severe=0 quality=good",
        ],
    )
    add_bullet(doc, "epoch：当前样本属于哪一次控制周期，便于过滤旧数据和复盘命令影响。")
    add_bullet(doc, "settling=1：仍处于播放器过渡期，只观察、不累计、不纠偏。")
    add_bullet(doc, "window_ready=1：样本数量达到最小要求，稳健统计才可供控制算法使用。")
    add_bullet(doc, "quality：窗口未 ready 时仅供观察；ready 后以 median_abs_diff_ms 判定。")

    doc.add_heading("4.6 本地回归结果", level=2)
    add_numbered(doc, "Windows x64 libVLC 完整目标再次构建通过。")
    add_numbered(doc, "WSL Ubuntu 22.04 的 SyncCinemaServer 目标再次构建通过。")
    add_numbered(doc, "两个脚本 TCP client 并发发送 PLAY 和 REPORT，epoch 1 的前 2 秒样本正确标记为 settling。")
    add_numbered(doc, "稳定期后窗口从 1 增长到 6，window_ready 变为 1，中位数约 60-61 ms、P95 约 75-87 ms。")
    add_numbered(doc, "发送 SEEK 后进入 epoch 2，旧窗口被清空并重新进入 settling，验证跨控制周期污染已被阻断。")
    add_callout(
        doc,
        "当前状态",
        "本阶段代码已经实现并完成本地构建与功能回归，但按提交节奏要求暂未提交或部署。"
        "云服务器当前仍运行已发布的 2c1bf4f 版本，便于清晰区分已发布基线与本地候选版本。",
    )

    doc.add_heading("4.7 尚未完成", level=2)
    add_bullet(doc, "窗口指标目前只用于日志分析，尚未自动下发校正命令。")
    add_bullet(doc, "新 client 的晚加入状态快照仍未实现，这是进入自动纠偏前更确定、优先级更高的功能缺口。")
    add_bullet(doc, "当前 P95 使用最近 12 个样本的 nearest-rank 算法，样本较少时粒度有限。")
    add_bullet(doc, "暂停和播放共用 2 秒稳定期，后续可按命令类型和播放器事件细化。")

    doc.add_heading("5. 面试表达模板", level=1)
    add_body(
        doc,
        "第一段可以这样说明指标演进：最初我用服务器 Room 的理论时钟评估每个客户端，"
        "但公网测试发现两端都可能因 libVLC 缓冲共同落后 Room 几百毫秒，"
        "这个指标不能直接代表观众之间不同步。于是我保留 client-to-Room 指标用于诊断，"
        "另外缓存每个 client 的最近上报，结合 RTT/2 和 server steady_clock，"
        "把异步到达的进度投影到同一时刻后计算 pair_diff。"
        "同时过滤陈旧样本和状态不一致样本，并坚持先观测后控制，"
        "为后续自动纠偏建立可信数据基础。"
    )
    add_body(
        doc,
        "第二段可以这样说明稳健性：公网实测中，两台客户端暂停后只差约 2 ms，"
        "但播放时单条日志会出现约 250 ms 的量化台阶。我没有直接按单个样本 seek，"
        "而是为每次 PLAY、PAUSE、SEEK 建立独立 epoch，清理旧进度统计但保留 RTT；"
        "控制后设置 2 秒稳定期，再用最近 12 个样本的中位数评估典型偏差、P95 观察尾部风险，"
        "同时统计连续严重异常。这样未来的校正策略会基于趋势，而不是被播放器异步行为或一次网络抖动误触发。"
    )

    doc.add_heading("6. 后续学习重点", level=1)
    add_numbered(doc, "SyncMetricsCollector::recordProgressReport：共享状态、快照、时间投影和日志输出如何分层。")
    add_numbered(doc, "projectPositionToServerTime：为什么 Playing 才能按经过时间推进。")
    add_numbered(doc, "SyncMetricsCollector::beginControlEpoch：为什么 RTT 保留，而播放偏差必须按控制周期重置。")
    add_numbered(doc, "medianOf / percentile95OfAbsoluteDiffs：稳健统计如何减少离群样本误导。")
    add_numbered(doc, "SyncServer::processLine：controlCommandMutex 如何统一 Room、广播、epoch 和 REPORT 的并发顺序。")
    add_numbered(doc, "Room::getEstimatedStateLocked：服务器如何用基准位置与 steady_clock 维护权威状态。")
    add_numbered(doc, "SyncMetricsCollector::recordPongReceived：为什么 RTT 可以只用 server 自己的时钟计算。")
    add_numbered(doc, "Client.cpp 的 playerMutex/sendMutex：多个线程如何安全共享播放器和同一个 TCP 字节流。")

    doc.add_heading("7. 后续工程路线", level=1)
    add_numbered(doc, "在合理提交间隔后提交并部署 epoch/window 候选版本，再用两台真实设备复测 PLAY、PAUSE 与 SEEK。")
    add_numbered(doc, "实现新 client 的状态快照与加入同步，先消除“晚加入后停在 0 秒”的确定性缺陷。")
    add_numbered(doc, "设计只读校正建议日志：根据 window_ready、中位数、P95 和连续异常输出 would_correct，但仍不实际控制。")
    add_numbered(doc, "验证建议策略后，再加入带阈值、冷却和迟滞的自动 seek；小偏差优先观察，暂不贸然引入倍速控制。")
    add_numbered(doc, "把网络与同步核心从 CLI 交互中进一步解耦，再接入 Qt 界面、聊天和弹幕。")
    add_numbered(doc, "补齐 systemd 服务、配置文件、日志轮转、自动构建测试和发布包。")

    add_callout(
        doc,
        "下一阶段入口",
        "先保持当前候选代码不提交，复核文档与本地回归证据。下一次工程阶段优先实现晚加入同步，"
        "随后再让稳健窗口输出“只建议、不执行”的校正决策；不要直接根据 client-to-Room 或单条 pair_diff 触发 seek。",
    )

    doc.save(OUTPUT_PATH)
    print(f"Created {OUTPUT_PATH}")


if __name__ == "__main__":
    build_document()
