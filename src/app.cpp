// app.exe - views data and results. Read-only: it never trains and never writes.

#include "framework.h"
#include "resource.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "bars.h"
#include "common.h"

#pragma comment(lib, "shell32.lib")

namespace
{
	constexpr uint32_t COLOUR_BACKGROUND = 0x00101418;
	constexpr uint32_t COLOUR_GRID = 0x00202830;
	constexpr uint32_t COLOUR_PRICE = 0x00d8dee4;
	constexpr uint32_t COLOUR_LONG = 0x0038c172;
	constexpr uint32_t COLOUR_SHORT = 0x00e05252;
	constexpr uint32_t COLOUR_FLAT = 0x00404850;
	constexpr uint32_t COLOUR_EQUITY = 0x0046b8dd;
	constexpr uint32_t COLOUR_DRAWDOWN = 0x00303a44;
	constexpr uint32_t COLOUR_CURSOR = 0x00606c78;

	constexpr uint32_t COLOUR_MAP_BACKGROUND = 0x00161b20;
	constexpr uint32_t COLOUR_MAP_PRICE = 0x004e5860;
	constexpr uint32_t COLOUR_MAP_SHADE = 0x000c0f12;
	constexpr uint32_t COLOUR_MAP_THUMB = 0x001f2933;
	constexpr uint32_t COLOUR_MAP_EDGE = 0x007d8b99;

	constexpr int8_t NO_POSITION = -2;

	struct pane
	{
		int top = 0;
		int height = 0;
		int bottom() const { return top + height; }
		bool contains(const int y) const { return y >= top && y < bottom(); }
	};

	struct surface
	{
		uint32_t* pixels = nullptr;
		int width = 0;
		int height = 0;

		void dot(const int x, const int y, const uint32_t colour) const
		{
			if (x < 0 || y < 0 || x >= width || y >= height) return;
			pixels[static_cast<size_t>(y) * width + x] = colour;
		}

		void column(const int x, int y0, int y1, const uint32_t colour) const
		{
			if (y0 > y1) std::swap(y0, y1);
			for (int y = y0; y <= y1; y += 1) dot(x, y, colour);
		}

		void fill(const int x0, const int y0, const int x1, const int y1, const uint32_t colour) const
		{
			for (int y = y0; y <= y1; y += 1)
				for (int x = x0; x <= x1; x += 1) dot(x, y, colour);
		}
	};

	// Everything loaded from disk. The viewer holds it and only reads it.
	struct view_model
	{
		bar_series series;
		std::vector<double> equity;    // per bar, NaN where the run had no position
		std::vector<int8_t> position;  // per bar, NO_POSITION where absent
		std::string bars_path;
		std::string run_id;
		std::string status;
		bool has_run = false;

		int64_t start = 0;
		int64_t span = 0;

		bool loaded() const { return !series.bars.empty(); }
	};

	// One column per pixel of the whole series, so dragging the mini-map does
	// not walk hundreds of thousands of bars per frame.
	struct minimap_cache
	{
		int width = 0;
		bool has_run = false;
		std::vector<double> low, high;
		std::vector<int8_t> side;

		bool matches(const int w, const bool run) const { return width == w && has_run == run && w > 0; }
	};

	enum class drag_mode
	{
		none,
		chart,
		minimap
	};

	view_model model;
	minimap_cache map_cache;
	HINSTANCE instance;
	HFONT mono_font;
	int cursor_x = -1;
	int cursor_y = -1;
	int dpi = 96;
	drag_mode dragging = drag_mode::none;
	int drag_x = 0;
	int64_t drag_start = 0;

	pane price_pane, band_pane, equity_pane, map_pane;

	int scaled(const int value) { return MulDiv(value, dpi, 96); }

	HFONT create_font()
	{
		return CreateFontW(scaled(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			FIXED_PITCH | FF_MODERN, L"Consolas");
	}

	std::string narrow(const std::wstring& text)
	{
		if (text.empty()) return {};
		const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string out(size > 0 ? size - 1 : 0, '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
		return out;
	}

	std::wstring widen(const std::string& text)
	{
		if (text.empty()) return {};
		const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
		std::wstring out(size > 0 ? size - 1 : 0, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
		return out;
	}

	void load_run(const std::string& directory)
	{
		const std::string path = directory + "/equity.csv";
		std::ifstream file(path);
		if (!file)
		{
			model.status = "no equity.csv in " + directory;
			return;
		}

		model.equity.assign(model.series.bars.size(), std::nan(""));
		model.position.assign(model.series.bars.size(), NO_POSITION);

		const int64_t origin = model.series.bars.front().time;
		const int64_t interval = model.series.interval_ms;

		std::string line;
		std::getline(file, line);
		size_t matched = 0;

		while (std::getline(file, line))
		{
			const std::vector<std::string> cells = split(line, ',');
			if (cells.size() < 6) continue;

			const int64_t time = strtoll(cells[0].c_str(), nullptr, 10);
			const int64_t index = interval > 0 ? (time - origin) / interval : -1;
			if (index < 0 || index >= static_cast<int64_t>(model.series.bars.size())) continue;

			model.position[static_cast<size_t>(index)] = static_cast<int8_t>(strtol(cells[2].c_str(), nullptr, 10));
			model.equity[static_cast<size_t>(index)] = strtod(cells[5].c_str(), nullptr);
			matched += 1;
		}

		model.has_run = matched > 0;
		model.run_id = directory;
		if (!model.has_run) model.status = "equity.csv did not line up with these bars";
	}

	void load_from_command_line()
	{
		std::string bars_path = "data/bars/bars.bin";
		std::string run_path;

		int count = 0;
		if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count))
		{
			for (int i = 1; i < count; i += 1)
			{
				const std::wstring token = argv[i];
				if (token == L"--bars" && i + 1 < count) bars_path = narrow(argv[++i]);
				else if (token == L"--run" && i + 1 < count) run_path = narrow(argv[++i]);
				else if (token.rfind(L"--", 0) != 0) bars_path = narrow(token);
			}
			LocalFree(argv);
		}

		model.bars_path = bars_path;

		std::string error;
		if (!read_bars(bars_path, model.series, &error))
		{
			model.status = error + "  -  build one with: cli bars --in data/raw --out " + bars_path;
			return;
		}

		model.span = std::min<int64_t>(static_cast<int64_t>(model.series.bars.size()), 20000);
		model.start = 0;
		model.status = model.series.symbol + "  " + std::to_string(model.series.bars.size()) + " bars";

		if (!run_path.empty()) load_run(run_path);
	}

	void clamp_view(const int width)
	{
		const auto total = static_cast<int64_t>(model.series.bars.size());
		if (total == 0) return;

		const int64_t minimum = std::min<int64_t>(total, std::max(64, width / 8));
		model.span = std::clamp<int64_t>(model.span, minimum, total);
		model.start = std::clamp<int64_t>(model.start, 0, total - model.span);
	}

	void layout(const int width, const int height)
	{
		const int header = scaled(44);
		const int footer = scaled(20);
		const int band = scaled(12);
		const int map = scaled(52);
		const int gap = scaled(6);

		map_pane.height = model.loaded() ? map : 0;
		map_pane.top = height - footer - map_pane.height;

		const int usable = map_pane.top - header - band - gap * 2;

		price_pane.top = header;
		price_pane.height = model.has_run ? usable * 62 / 100 : usable;
		band_pane.top = price_pane.bottom() + scaled(2);
		band_pane.height = model.has_run ? band : 0;
		equity_pane.top = band_pane.bottom() + gap;
		equity_pane.height = model.has_run ? usable - price_pane.height : 0;

		(void)width;
	}

	// Aggregates the whole series into one column per pixel. Rebuilt only when
	// the width changes, so mini-map dragging stays smooth on a million bars.
	void build_minimap(const int width)
	{
		if (map_cache.matches(width, model.has_run) || !model.loaded()) return;

		map_cache.width = width;
		map_cache.has_run = model.has_run;
		map_cache.low.assign(width, 0.0);
		map_cache.high.assign(width, 0.0);
		map_cache.side.assign(width, NO_POSITION);

		const auto total = static_cast<size_t>(model.series.bars.size());

		for (int x = 0; x < width; x += 1)
		{
			const size_t from = total * x / width;
			const size_t to = std::max(from + 1, total * (x + 1) / width);

			double low = 1e300;
			double high = -1e300;
			int longs = 0;
			int shorts = 0;
			int flats = 0;

			for (size_t i = from; i < to && i < total; i += 1)
			{
				low = std::min(low, model.series.bars[i].low);
				high = std::max(high, model.series.bars[i].high);

				if (!model.has_run || model.position[i] == NO_POSITION) continue;
				if (model.position[i] > 0) longs += 1;
				else if (model.position[i] < 0) shorts += 1;
				else flats += 1;
			}

			map_cache.low[x] = low;
			map_cache.high[x] = high;

			if (longs + shorts + flats > 0)
			{
				map_cache.side[x] = longs >= shorts && longs >= flats ? 1
					: (shorts >= flats ? -1 : 0);
			}
		}
	}

	void render_minimap(const surface& s)
	{
		if (map_pane.height <= 0 || !model.loaded()) return;

		const int width = s.width;
		const int top = map_pane.top;
		const int bottom = map_pane.bottom() - 1;

		s.fill(0, top, width - 1, bottom, COLOUR_MAP_BACKGROUND);

		double low = 1e300;
		double high = -1e300;
		for (int x = 0; x < width; x += 1)
		{
			low = std::min(low, map_cache.low[x]);
			high = std::max(high, map_cache.high[x]);
		}
		if (high <= low) high = low + 1.0;

		const auto total = static_cast<double>(model.series.bars.size());
		const int thumb_left = static_cast<int>(model.start / total * width);
		const int thumb_right = static_cast<int>((model.start + model.span) / total * width);

		// The viewport reads as a lit window over a dimmed whole.
		s.fill(std::max(0, thumb_left), top, std::min(width - 1, thumb_right), bottom, COLOUR_MAP_THUMB);

		const int inset = scaled(3);
		const int plot_top = top + inset;
		const int plot_bottom = bottom - inset;
		const int plot_height = std::max(1, plot_bottom - plot_top);

		for (int x = 0; x < width; x += 1)
		{
			const bool inside = x >= thumb_left && x <= thumb_right;

			const int y_high = plot_bottom - static_cast<int>((map_cache.high[x] - low) / (high - low) * plot_height);
			const int y_low = plot_bottom - static_cast<int>((map_cache.low[x] - low) / (high - low) * plot_height);

			uint32_t colour = inside ? COLOUR_PRICE : COLOUR_MAP_PRICE;
			if (map_cache.side[x] == 1) colour = COLOUR_LONG;
			else if (map_cache.side[x] == -1) colour = COLOUR_SHORT;

			s.column(x, y_high, y_low, colour);
			if (!inside) s.dot(x, top, COLOUR_MAP_SHADE);
		}

		s.column(std::max(0, thumb_left), top, bottom, COLOUR_MAP_EDGE);
		s.column(std::min(width - 1, thumb_right), top, bottom, COLOUR_MAP_EDGE);
		for (int x = 0; x < width; x += 1) s.dot(x, top, COLOUR_GRID);
	}

	// Centres the view on the clicked position in the mini-map.
	void minimap_seek(const int x, const int width)
	{
		if (!model.loaded() || width <= 0) return;

		const auto total = static_cast<int64_t>(model.series.bars.size());
		const int64_t centre = static_cast<int64_t>(static_cast<double>(x) / width * total);
		model.start = centre - model.span / 2;
		clamp_view(width);
	}

	void draw_text(const HDC dc, const int x, const int y, const std::string& text, const COLORREF colour)
	{
		SetTextColor(dc, colour);
		const std::wstring wide = widen(text);
		TextOutW(dc, x, y, wide.c_str(), static_cast<int>(wide.size()));
	}

	std::string format_number(const double value, const int decimals)
	{
		char buf[64];
		snprintf(buf, sizeof buf, "%.*f", decimals, value);
		return buf;
	}

	void render(const surface& s, const HDC dc)
	{
		s.fill(0, 0, s.width - 1, s.height - 1, COLOUR_BACKGROUND);

		if (!model.loaded())
		{
			draw_text(dc, scaled(16), scaled(16), "spq viewer", RGB(0xd8, 0xde, 0xe4));
			draw_text(dc, scaled(16), scaled(40), model.status, RGB(0xe0, 0x82, 0x52));
			return;
		}

		layout(s.width, s.height);
		build_minimap(s.width);

		const pane& price = price_pane;
		const pane& band = band_pane;
		const pane& equity = equity_pane;

		const int width = s.width;
		const auto first = static_cast<size_t>(model.start);
		const auto count = static_cast<size_t>(model.span);

		// One pixel column per screen column, aggregating the bars that fall in it.
		double price_low = 1e300;
		double price_high = -1e300;
		double equity_low = 1e300;
		double equity_high = -1e300;

		for (size_t i = first; i < first + count; i += 1)
		{
			price_low = std::min(price_low, model.series.bars[i].low);
			price_high = std::max(price_high, model.series.bars[i].high);

			if (model.has_run && !std::isnan(model.equity[i]))
			{
				equity_low = std::min(equity_low, model.equity[i]);
				equity_high = std::max(equity_high, model.equity[i]);
			}
		}

		if (price_high <= price_low) price_high = price_low + 1.0;
		if (equity_high <= equity_low) { equity_low = -0.001; equity_high = 0.001; }

		const auto to_y = [](const double value, const double low, const double high, const pane& p)
			{
				const double t = (value - low) / (high - low);
				return p.bottom() - static_cast<int>(t * (p.height - 1));
			};

		for (int g = 1; g < 4; g += 1)
		{
			const int y = price.top + price.height * g / 4;
			for (int x = 0; x < width; x += 3) s.dot(x, y, COLOUR_GRID);
		}

		int previous_price_y = -1;
		int previous_equity_y = -1;

		for (int x = 0; x < width; x += 1)
		{
			const size_t from = first + static_cast<size_t>(count * x / width);
			const size_t to = std::min(first + count, first + static_cast<size_t>(count * (x + 1) / width));
			if (to <= from) continue;

			double low = 1e300;
			double high = -1e300;
			double close = model.series.bars[to - 1].close;
			int longs = 0;
			int shorts = 0;
			int flats = 0;
			double last_equity = std::nan("");

			for (size_t i = from; i < to; i += 1)
			{
				low = std::min(low, model.series.bars[i].low);
				high = std::max(high, model.series.bars[i].high);

				if (!model.has_run) continue;

				if (model.position[i] == NO_POSITION) continue;
				if (model.position[i] > 0) longs += 1;
				else if (model.position[i] < 0) shorts += 1;
				else flats += 1;

				if (!std::isnan(model.equity[i])) last_equity = model.equity[i];
			}

			const int y_low = to_y(low, price_low, price_high, price);
			const int y_high = to_y(high, price_low, price_high, price);
			s.column(x, y_high, y_low, COLOUR_PRICE);

			const int y_close = to_y(close, price_low, price_high, price);
			if (previous_price_y >= 0) s.column(x, previous_price_y, y_close, COLOUR_PRICE);
			previous_price_y = y_close;

			if (!model.has_run) continue;

			uint32_t band_colour = COLOUR_BACKGROUND;
			if (longs + shorts + flats > 0)
			{
				band_colour = longs >= shorts && longs >= flats ? COLOUR_LONG
					: (shorts >= flats ? COLOUR_SHORT : COLOUR_FLAT);
			}
			s.column(x, band.top, band.bottom() - 1, band_colour);

			if (std::isnan(last_equity)) continue;

			const int y_equity = to_y(last_equity, equity_low, equity_high, equity);
			const int y_zero = to_y(0.0, equity_low, equity_high, equity);
			s.column(x, std::min(y_equity, y_zero), std::max(y_equity, y_zero), COLOUR_DRAWDOWN);

			if (previous_equity_y >= 0) s.column(x, previous_equity_y, y_equity, COLOUR_EQUITY);
			previous_equity_y = y_equity;
			s.dot(x, y_equity, COLOUR_EQUITY);
		}

		if (cursor_x >= 0 && cursor_x < width && !map_pane.contains(cursor_y))
		{
			for (int y = price.top; y < map_pane.top; y += 2) s.dot(cursor_x, y, COLOUR_CURSOR);
		}

		render_minimap(s);

		const size_t hovered = cursor_x >= 0
			? std::min(first + count - 1, first + static_cast<size_t>(count * cursor_x / width))
			: first + count - 1;

		draw_text(dc, scaled(10), scaled(6), model.series.symbol + "  " + model.bars_path
			+ "   bars " + std::to_string(first) + ".." + std::to_string(first + count)
			+ " of " + std::to_string(model.series.bars.size()),
			RGB(0xd8, 0xde, 0xe4));

		std::string second_line = format_time_ms(model.series.bars[hovered].time)
			+ "   price " + format_number(model.series.bars[hovered].close, 2);

		if (model.has_run)
		{
			const int8_t p = model.position[hovered];
			second_line += "   position ";
			second_line += p == NO_POSITION ? "-" : (p > 0 ? "long" : (p < 0 ? "short" : "flat"));

			if (!std::isnan(model.equity[hovered]))
			{
				second_line += "   equity " + format_number(model.equity[hovered] * 100.0, 3) + "%";
			}
		}

		draw_text(dc, scaled(10), scaled(24), second_line, RGB(0x9a, 0xa4, 0xae));

		const std::string footer_text = model.has_run
			? "run " + model.run_id + "   green long, red short, grey flat   drag or middle-drag to pan, wheel zoom, shift-wheel scroll, click the map below"
			: "no run loaded - pass --run runs/<id>   drag to pan, wheel zoom, shift-wheel scroll, click the map below";
		draw_text(dc, scaled(10), s.height - scaled(18), footer_text, RGB(0x6a, 0x74, 0x7e));
	}
}

ATOM register_class(HINSTANCE);
BOOL create_main_window(HINSTANCE, int);
LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK about_proc(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow)
{
	instance = hInstance;
	load_from_command_line();

	register_class(hInstance);
	if (!create_main_window(hInstance, nCmdShow)) return FALSE;

	const HACCEL accelerators = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_APP));
	MSG message;

	while (GetMessage(&message, nullptr, 0, 0))
	{
		if (!TranslateAccelerator(message.hwnd, accelerators, &message))
		{
			TranslateMessage(&message);
			DispatchMessage(&message);
		}
	}

	DeleteObject(mono_font);
	return static_cast<int>(message.wParam);
}

ATOM register_class(const HINSTANCE hInstance)
{
	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof wcex;
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = window_proc;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP));
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = nullptr;
	wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_APP);
	wcex.lpszClassName = L"spqViewer";
	wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));

	return RegisterClassExW(&wcex);
}

BOOL create_main_window(const HINSTANCE hInstance, const int nCmdShow)
{
	const HWND window = CreateWindowW(L"spqViewer", L"spq viewer", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 0, 100, 100, nullptr, nullptr, hInstance, nullptr);

	if (!window) return FALSE;

	dpi = static_cast<int>(GetDpiForWindow(window));

	// Sized after the window exists so the default is 1400x900 logical pixels
	// on any monitor, rather than 1400x900 physical on a 4K one.
	SetWindowPos(window, nullptr, 0, 0, scaled(1400), scaled(900),
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	ShowWindow(window, nCmdShow);
	UpdateWindow(window);
	return TRUE;
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
{
	static HDC memory_dc = nullptr;
	static HBITMAP memory_bitmap = nullptr;
	static HGDIOBJ previous_bitmap = nullptr;
	static surface canvas;
	static int font_dpi = 0;

	switch (message)
	{
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDM_ABOUT:
			DialogBox(instance, MAKEINTRESOURCE(IDD_ABOUTBOX), window, about_proc);
			return 0;
		case IDM_EXIT:
			DestroyWindow(window);
			return 0;
		default:
			return DefWindowProc(window, message, wParam, lParam);
		}

	case WM_MOUSEMOVE:
	{
		const int x = GET_X_LPARAM(lParam);
		cursor_x = x;
		cursor_y = GET_Y_LPARAM(lParam);

		if (dragging == drag_mode::chart && canvas.width > 0)
		{
			const int64_t moved = static_cast<int64_t>(drag_x - x) * model.span / canvas.width;
			model.start = drag_start + moved;
			clamp_view(canvas.width);
		}
		else if (dragging == drag_mode::minimap && canvas.width > 0)
		{
			const auto total = static_cast<int64_t>(model.series.bars.size());
			model.start = drag_start + static_cast<int64_t>(x - drag_x) * total / canvas.width;
			clamp_view(canvas.width);
		}

		InvalidateRect(window, nullptr, FALSE);
		return 0;
	}

	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	{
		const int x = GET_X_LPARAM(lParam);
		const int y = GET_Y_LPARAM(lParam);
		drag_x = x;

		if (message == WM_LBUTTONDOWN && map_pane.contains(y) && canvas.width > 0)
		{
			minimap_seek(x, canvas.width);
			dragging = drag_mode::minimap;
		}
		else
		{
			dragging = drag_mode::chart;
		}

		drag_start = model.start;
		SetCapture(window);
		InvalidateRect(window, nullptr, FALSE);
		return 0;
	}

	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
		dragging = drag_mode::none;
		ReleaseCapture();
		return 0;

	case WM_MOUSEHWHEEL:
	{
		if (!model.loaded() || canvas.width <= 0) return 0;
		model.start += GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? model.span / 8 : -model.span / 8;
		clamp_view(canvas.width);
		InvalidateRect(window, nullptr, FALSE);
		return 0;
	}

	case WM_MOUSEWHEEL:
	{
		if (!model.loaded() || canvas.width <= 0) return 0;

		const int delta = GET_WHEEL_DELTA_WPARAM(wParam);

		// Shift turns the wheel into a horizontal scroll, as in most editors.
		if (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT)
		{
			model.start += delta > 0 ? -model.span / 8 : model.span / 8;
			clamp_view(canvas.width);
			InvalidateRect(window, nullptr, FALSE);
			return 0;
		}

		POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(window, &point);

		const double anchor = std::clamp(static_cast<double>(point.x) / canvas.width, 0.0, 1.0);
		const int64_t focus = model.start + static_cast<int64_t>(anchor * model.span);
		const double factor = delta > 0 ? 0.8 : 1.25;

		model.span = static_cast<int64_t>(model.span * factor);
		clamp_view(canvas.width);
		model.start = focus - static_cast<int64_t>(anchor * model.span);
		clamp_view(canvas.width);

		InvalidateRect(window, nullptr, FALSE);
		return 0;
	}

	case WM_SETCURSOR:
		if (LOWORD(lParam) == HTCLIENT)
		{
			SetCursor(LoadCursor(nullptr, dragging != drag_mode::none ? IDC_SIZEWE : IDC_ARROW));
			return TRUE;
		}
		return DefWindowProc(window, message, wParam, lParam);

	case WM_DPICHANGED:
	{
		dpi = HIWORD(wParam);

		const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
		SetWindowPos(window, nullptr, suggested->left, suggested->top,
			suggested->right - suggested->left, suggested->bottom - suggested->top,
			SWP_NOZORDER | SWP_NOACTIVATE);

		canvas.width = 0;   // forces the backing bitmap and the font to be rebuilt
		InvalidateRect(window, nullptr, TRUE);
		return 0;
	}

	case WM_KEYDOWN:
		if (!model.loaded()) return 0;
		switch (wParam)
		{
		case VK_HOME:
			model.start = 0;
			model.span = static_cast<int64_t>(model.series.bars.size());
			break;
		case VK_LEFT:
			model.start -= model.span / 8;
			break;
		case VK_RIGHT:
			model.start += model.span / 8;
			break;
		case VK_END:
			model.start = static_cast<int64_t>(model.series.bars.size());
			break;
		default:
			return 0;
		}
		clamp_view(canvas.width);
		InvalidateRect(window, nullptr, FALSE);
		return 0;

	case WM_PAINT:
	{
		PAINTSTRUCT paint;
		const HDC dc = BeginPaint(window, &paint);

		RECT client;
		GetClientRect(window, &client);
		const int width = std::max<int>(client.right, 1);
		const int height = std::max<int>(client.bottom, 1);

		if (width != canvas.width || height != canvas.height)
		{
			if (memory_dc)
			{
				SelectObject(memory_dc, previous_bitmap);
				DeleteObject(memory_bitmap);
				DeleteDC(memory_dc);
			}

			// Only safe once the DC holding it is gone.
			if (font_dpi != dpi)
			{
				DeleteObject(mono_font);
				mono_font = create_font();
				font_dpi = dpi;
			}

			BITMAPINFO info = {};
			info.bmiHeader.biSize = sizeof info.bmiHeader;
			info.bmiHeader.biWidth = width;
			info.bmiHeader.biHeight = -height;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;

			void* bits = nullptr;
			memory_dc = CreateCompatibleDC(dc);
			memory_bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			previous_bitmap = SelectObject(memory_dc, memory_bitmap);

			canvas.pixels = static_cast<uint32_t*>(bits);
			canvas.width = width;
			canvas.height = height;

			SelectObject(memory_dc, mono_font);
			SetBkMode(memory_dc, TRANSPARENT);
			clamp_view(width);
		}

		if (canvas.pixels) render(canvas, memory_dc);
		BitBlt(dc, 0, 0, width, height, memory_dc, 0, 0, SRCCOPY);

		EndPaint(window, &paint);
		return 0;
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_DESTROY:
		if (memory_dc)
		{
			SelectObject(memory_dc, previous_bitmap);
			DeleteObject(memory_bitmap);
			DeleteDC(memory_dc);
			memory_dc = nullptr;
		}
		PostQuitMessage(0);
		return 0;

	default:
		return DefWindowProc(window, message, wParam, lParam);
	}
}

INT_PTR CALLBACK about_proc(const HWND dialog, const UINT message, const WPARAM wParam, LPARAM)
{
	switch (message)
	{
	case WM_INITDIALOG:
		return TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(dialog, LOWORD(wParam));
			return TRUE;
		}
		break;

	default:
		break;
	}

	return FALSE;
}
