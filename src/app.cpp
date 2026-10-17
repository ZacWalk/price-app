// app.exe - views data and results. Read-only: it never trains and never writes.

#include "platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "bars.h"
#include "common.h"

namespace
{
	// Packed 0x00RRGGBB, the layout of the 32-bit bitmap the chart is drawn into.
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

	constexpr pf::color_t TEXT_HEAD(0xd8, 0xde, 0xe4);
	constexpr pf::color_t TEXT_BODY(0x9a, 0xa4, 0xae);
	constexpr pf::color_t TEXT_FOOT(0x6a, 0x74, 0x7e);
	constexpr pf::color_t TEXT_WARN(0xe0, 0x82, 0x52);

	constexpr int8_t NO_POSITION = -2;

	// Logical pixels above the price pane (two text lines) and below the mini-map.
	constexpr int HEADER_HEIGHT = 44;
	constexpr int FOOTER_HEIGHT = 20;

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

		// Spans are clipped before the loop: an off-surface coordinate must cost
		// nothing, not one bounds check per row out to INT_MIN.
		void column(const int x, int y0, int y1, const uint32_t colour) const
		{
			if (y0 > y1) std::swap(y0, y1);
			if (x < 0 || x >= width) return;
			y0 = std::max(y0, 0);
			y1 = std::min(y1, height - 1);
			for (int y = y0; y <= y1; y += 1) pixels[static_cast<size_t>(y) * width + x] = colour;
		}

		void fill(int x0, int y0, int x1, int y1, const uint32_t colour) const
		{
			x0 = std::max(x0, 0);
			y0 = std::max(y0, 0);
			x1 = std::min(x1, width - 1);
			y1 = std::min(y1, height - 1);
			for (int y = y0; y <= y1; y += 1)
				for (int x = x0; x <= x1; x += 1) pixels[static_cast<size_t>(y) * width + x] = colour;
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
		std::string run_problem;       // why --run could not be shown, drawn in the footer
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
	pf::bitmap canvas_bitmap;
	surface canvas;
	pf::font_handle mono_font = 0;
	int mono_font_px = 0;
	int cursor_x = -1;
	int cursor_y = -1;
	double dpi_scale = 1.0;
	drag_mode dragging = drag_mode::none;
	int drag_x = 0;
	int64_t drag_start = 0;

	pane price_pane, band_pane, equity_pane, map_pane;

	bool offscreen = false;
	int paints = 0;

	int scaled(const int value) { return static_cast<int>(value * dpi_scale + 0.5); }

	void ensure_font()
	{
		const int px = scaled(14);
		if (mono_font && px == mono_font_px) return;

		if (mono_font) pf::delete_font_handle(mono_font);

		pf::font_def def;
		def.face = "Consolas";
		def.size = px;
		mono_font = pf::create_font_handle(def);
		mono_font_px = px;
	}

	void load_run(const std::string& directory)
	{
		// A run is drawn against bar times, so there is nothing to line it up with.
		if (model.series.bars.empty()) return;

		const std::string path = directory + "/equity.csv";
		std::ifstream file(path);
		if (!file)
		{
			model.status = "no equity.csv in " + directory;
			model.run_problem = model.status;
			return;
		}

		model.equity.assign(model.series.bars.size(), std::nan(""));
		model.position.assign(model.series.bars.size(), NO_POSITION);

		std::string line;
		std::getline(file, line);
		size_t matched = 0;

		while (std::getline(file, line))
		{
			const std::vector<std::string> cells = split(line, ',');
			if (cells.size() < 6) continue;

			const int64_t time = strtoll(cells[0].c_str(), nullptr, 10);
			const size_t index = bar_index_at_time(model.series, time);
			if (index == model.series.size()) continue;

			const double equity = strtod(cells[5].c_str(), nullptr);
			model.position[static_cast<size_t>(index)] = static_cast<int8_t>(strtol(cells[2].c_str(), nullptr, 10));
			model.equity[static_cast<size_t>(index)] = std::isfinite(equity) ? equity : std::nan("");
			matched += 1;
		}

		model.has_run = matched > 0;
		model.run_id = directory;
		if (!model.has_run)
		{
			model.status = "equity.csv did not line up with these bars";
			model.run_problem = "equity.csv in " + directory + " did not line up with these bars";
		}
	}

	void load_from_params(const std::span<const std::string_view> params)
	{
		std::string bars_path = "data/bars/bars.bin";
		std::string run_path;
		std::string problem;

		// A typo must not turn its value into the bar file: `--rn runs\x` once
		// reported "cannot open runs\x - build one with: cli bars ... --out runs\x".
		for (size_t i = 0; i < params.size(); i += 1)
		{
			const std::string_view token = params[i];
			const bool valued = token == "--bars" || token == "--run";
			if (valued && (i + 1 >= params.size() || params[i + 1].starts_with("--")))
				problem = std::string(token) + " needs a value";
			else if (token == "--bars") bars_path = params[++i];
			else if (token == "--run") run_path = params[++i];
			else if (token == "--offscreen") continue;
			else if (token.starts_with("--")) problem = "unknown option " + std::string(token);
			else bars_path = token;
		}

		model.bars_path = bars_path;
		if (!problem.empty())
		{
			model.status = problem + "  -  the viewer takes [--bars] <file> [--run <dir>]";
			return;
		}

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
		const int header = scaled(HEADER_HEIGHT);
		const int footer = scaled(FOOTER_HEIGHT);
		const int band = scaled(12);
		const int map = scaled(52);
		const int gap = scaled(6);

		// A short window gives up the mini-map before the chart it summarises, and
		// a pane with no room is hidden rather than drawn over its neighbours.
		const int chart = height - header - footer - band - gap * 2;
		map_pane.height = model.loaded() && chart - map >= map ? map : 0;
		map_pane.top = height - footer - map_pane.height;

		const int usable = std::max(0, map_pane.top - header - band - gap * 2);
		const bool split = model.has_run && usable > 0;

		price_pane.top = header;
		price_pane.height = split ? usable * 62 / 100 : usable;
		band_pane.top = price_pane.bottom() + scaled(2);
		band_pane.height = split ? band : 0;
		equity_pane.top = band_pane.bottom() + gap;
		equity_pane.height = split ? usable - price_pane.height : 0;

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
			bool priced = false;
			int longs = 0;
			int shorts = 0;
			int flats = 0;

			for (size_t i = from; i < to && i < total; i += 1)
			{
				const bar& b = model.series.bars[i];
				if (std::isfinite(b.low) && std::isfinite(b.high))
				{
					low = std::min(low, b.low);
					high = std::max(high, b.high);
					priced = true;
				}

				if (!model.has_run || model.position[i] == NO_POSITION) continue;
				if (model.position[i] > 0) longs += 1;
				else if (model.position[i] < 0) shorts += 1;
				else flats += 1;
			}

			// NaN marks a column with no finite price, so it is left undrawn.
			map_cache.low[x] = priced ? low : std::nan("");
			map_cache.high[x] = priced ? high : std::nan("");

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
			if (std::isnan(map_cache.low[x])) continue;
			low = std::min(low, map_cache.low[x]);
			high = std::max(high, map_cache.high[x]);
		}
		if (high < low) { low = 0.0; high = 1.0; }
		else if (high <= low) high = low + 1.0;

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
			if (!inside) s.dot(x, top, COLOUR_MAP_SHADE);
			if (std::isnan(map_cache.low[x])) continue;

			const int y_high = plot_bottom - static_cast<int>((map_cache.high[x] - low) / (high - low) * plot_height);
			const int y_low = plot_bottom - static_cast<int>((map_cache.low[x] - low) / (high - low) * plot_height);

			uint32_t colour = inside ? COLOUR_PRICE : COLOUR_MAP_PRICE;
			if (map_cache.side[x] == 1) colour = COLOUR_LONG;
			else if (map_cache.side[x] == -1) colour = COLOUR_SHORT;

			s.column(x, y_high, y_low, colour);
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

	void draw_label(pf::draw_context& dc, const int x, const int y, const std::string& text, const pf::color_t colour)
	{
		dc.draw_text_h(x, y, text, mono_font, colour);
	}

	std::string format_number(const double value, const int decimals)
	{
		char buf[64];
		snprintf(buf, sizeof buf, "%.*f", decimals, value);
		return buf;
	}

	std::string format_price(const double value)
	{
		if (!model.series.generic) return format_number(value, 2);
		char buf[64];
		snprintf(buf, sizeof buf, "%.8g", value);
		return buf;
	}

	// The chart is drawn as pixels into canvas_bitmap, blitted, then labelled;
	// text has to come last because the blit would otherwise cover it.
	void render(pf::draw_context& dc, const surface& s)
	{
		s.fill(0, 0, s.width - 1, s.height - 1, COLOUR_BACKGROUND);

		if (!model.loaded())
		{
			dc.draw_bitmap(0, 0, canvas_bitmap);
			draw_label(dc, scaled(16), scaled(16), "price-app viewer", TEXT_HEAD);
			draw_label(dc, scaled(16), scaled(40), model.status, TEXT_WARN);
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
		// Non-finite prices are skipped: they have no place on a scale.
		double price_low = 1e300;
		double price_high = -1e300;
		bool priced = false;
		double equity_low = 1e300;
		double equity_high = -1e300;

		for (size_t i = first; i < first + count; i += 1)
		{
			const bar& b = model.series.bars[i];
			if (std::isfinite(b.low) && std::isfinite(b.high))
			{
				price_low = std::min(price_low, b.low);
				price_high = std::max(price_high, b.high);
				priced = true;
			}

			if (model.has_run && !std::isnan(model.equity[i]))
			{
				equity_low = std::min(equity_low, model.equity[i]);
				equity_high = std::max(equity_high, model.equity[i]);
			}
		}

		if (!priced) { price_low = 0.0; price_high = 1.0; }
		else if (price_high <= price_low) price_high = price_low + 1.0;
		if (equity_high < equity_low) { equity_low = -0.001; equity_high = 0.001; }
		else if (equity_high <= equity_low) { equity_low -= 0.001; equity_high += 0.001; }

		// Clamped into the pane, so the zero line of an all-negative equity view
		// shades to the pane's edge instead of over the band and the price.
		const auto to_y = [](const double value, const double low, const double high, const pane& p)
			{
				double t = (value - low) / (high - low);
				if (!(t >= 0.0)) t = 0.0;
				else if (t > 1.0) t = 1.0;
				return p.bottom() - 1 - static_cast<int>(t * (p.height - 1));
			};

		if (price.height > 0)
		{
			for (int g = 1; g < 4; g += 1)
			{
				const int y = price.top + price.height * g / 4;
				for (int x = 0; x < width; x += 3) s.dot(x, y, COLOUR_GRID);
			}
		}

		int previous_price_y = -1;
		int previous_equity_y = -1;

		for (int x = 0; x < width; x += 1)
		{
			// Zoomed in past one bar per pixel, a column still shows the bar it
			// falls in; skipping it would stripe the band and the equity fill.
			const size_t from = first + static_cast<size_t>(count * x / width);
			const size_t to = std::min(first + count,
				std::max(from + 1, first + static_cast<size_t>(count * (x + 1) / width)));

			double low = 1e300;
			double high = -1e300;
			bool column_priced = false;
			double close = std::nan("");
			int longs = 0;
			int shorts = 0;
			int flats = 0;
			double last_equity = std::nan("");

			for (size_t i = from; i < to; i += 1)
			{
				const bar& b = model.series.bars[i];
				if (std::isfinite(b.low) && std::isfinite(b.high))
				{
					low = std::min(low, b.low);
					high = std::max(high, b.high);
					column_priced = true;
				}
				if (std::isfinite(b.close)) close = b.close;

				if (!model.has_run) continue;

				if (model.position[i] == NO_POSITION) continue;
				if (model.position[i] > 0) longs += 1;
				else if (model.position[i] < 0) shorts += 1;
				else flats += 1;

				if (!std::isnan(model.equity[i])) last_equity = model.equity[i];
			}

			if (price.height > 0)
			{
				if (column_priced)
				{
					const int y_low = to_y(low, price_low, price_high, price);
					const int y_high = to_y(high, price_low, price_high, price);
					s.column(x, y_high, y_low, COLOUR_PRICE);
				}

				if (!std::isnan(close))
				{
					const int y_close = to_y(close, price_low, price_high, price);
					if (previous_price_y >= 0) s.column(x, previous_price_y, y_close, COLOUR_PRICE);
					previous_price_y = y_close;
				}
			}

			if (!model.has_run || band.height <= 0) continue;

			uint32_t band_colour = COLOUR_BACKGROUND;
			if (longs + shorts + flats > 0)
			{
				band_colour = longs >= shorts && longs >= flats ? COLOUR_LONG
					: (shorts >= flats ? COLOUR_SHORT : COLOUR_FLAT);
			}
			s.column(x, band.top, band.bottom() - 1, band_colour);

			if (std::isnan(last_equity) || equity.height <= 0) continue;

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
		dc.draw_bitmap(0, 0, canvas_bitmap);

		const size_t hovered = cursor_x >= 0
			? std::min(first + count - 1, first + static_cast<size_t>(count * cursor_x / width))
			: first + count - 1;

		// The live range comes before the path, so a long path cannot cut its numbers
		// short: "bars 0..22855" once read as "bars 0..2285" at the window's edge.
		draw_label(dc, scaled(10), scaled(6), model.series.symbol
			+ "   bars " + std::to_string(first) + ".." + std::to_string(first + count)
			+ " of " + std::to_string(model.series.bars.size()) + "   " + model.bars_path,
			TEXT_HEAD);

		std::string second_line = format_time_ms(model.series.bars[hovered].time)
			+ "   price " + format_price(model.series.bars[hovered].close);
		if (model.series.generic)
			second_line += " " + model.series.market.currency + "   " + model.series.market.calendar
				+ "   " + model.series.market.price_basis;

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

		draw_label(dc, scaled(10), scaled(24), second_line, TEXT_BODY);

		// A window too short for both text bands keeps the header, not the overlap.
		if (s.height >= scaled(HEADER_HEIGHT + FOOTER_HEIGHT))
		{
			const std::string controls = "drag to pan, wheel zoom, shift-wheel scroll, click the map below";
			if (model.has_run)
				draw_label(dc, scaled(10), s.height - scaled(18), "run " + model.run_id
					+ "   green long, red short, grey flat   " + controls, TEXT_FOOT);
			else if (!model.run_problem.empty())
				draw_label(dc, scaled(10), s.height - scaled(18), model.run_problem + "   " + controls, TEXT_WARN);
			else
				draw_label(dc, scaled(10), s.height - scaled(18), "no run loaded - pass --run runs/<id>   " + controls, TEXT_FOOT);
		}
	}

	void ensure_canvas(const int width, const int height)
	{
		if (canvas_bitmap.width == width && canvas_bitmap.height == height) return;

		canvas_bitmap.width = width;
		canvas_bitmap.height = height;
		canvas_bitmap.pixels.assign(static_cast<size_t>(width) * height, COLOUR_BACKGROUND);

		canvas.pixels = canvas_bitmap.pixels.data();
		canvas.width = width;
		canvas.height = height;

		clamp_view(width);
	}

	void build_menu(const pf::window_frame_ptr& frame)
	{
		std::weak_ptr<pf::window_frame> weak = frame;

		pf::menu_command file_menu;
		file_menu.text = "File";
		file_menu.children.emplace_back("Exit", 105, [weak]
		{
			if (const auto f = weak.lock()) f->close();
		});

		pf::menu_command help_menu;
		help_menu.text = "Help";
		help_menu.children.emplace_back("About", 104, [weak]
		{
			if (const auto f = weak.lock())
				f->message_box("price-app viewer\nReads bars and position predictions. It never trains and never writes.",
				               "About price-app", pf::msg_box_style::ok);
		});

		frame->set_menu({std::move(file_menu), std::move(help_menu)});
	}

	class viewer_reactor : public pf::frame_reactor
	{
	public:
		uint32_t handle_message(pf::window_frame_ptr frame, const pf::message_type m,
		                        const pf::message_params&) override
		{
			switch (m)
			{
			case pf::message_type::create:
				dpi_scale = frame->get_dpi_scale();
				ensure_font();
				frame->set_text("price-app viewer");
				build_menu(frame);
				// An offscreen run keeps the position the platform parked it at.
				if (offscreen) frame->set_timer(k_offscreen_timer, 250);
				else frame->set_placement(startup_placement());
				return 0;

			case pf::message_type::timer:
				// Offscreen check: report what was painted, then leave.
				frame->kill_timer(k_offscreen_timer);
				pf::write_stdout(std::to_string(paints) + " paints  " +
					std::to_string(canvas.width) + "x" + std::to_string(canvas.height) + "  " +
					model.status + "\n");
				frame->close();
				return 0;

			case pf::message_type::dpi_changed:
				// The window moves to a monitor with a different scale; the new
				// scale is picked up on the next paint.
				frame->invalidate();
				return 0;

			case pf::message_type::erase_background:
				return 1;

			case pf::message_type::close:
				frame->close();
				return 0;

			default:
				return 0;
			}
		}

		uint32_t handle_mouse(pf::window_frame_ptr frame, const pf::mouse_message_type m,
		                      const pf::mouse_params& params) override
		{
			switch (m)
			{
			case pf::mouse_message_type::mouse_move:
				cursor_x = params.point.x;
				cursor_y = params.point.y;
				// Re-armed on every move: the readout must not stay on the last bar
				// hovered once the pointer has left the chart.
				frame->track_mouse_leave();

				if (dragging == drag_mode::chart && canvas.width > 0)
				{
					const int64_t moved = static_cast<int64_t>(drag_x - params.point.x) * model.span / canvas.width;
					model.start = drag_start + moved;
					clamp_view(canvas.width);
				}
				else if (dragging == drag_mode::minimap && canvas.width > 0)
				{
					const auto total = static_cast<int64_t>(model.series.bars.size());
					model.start = drag_start + static_cast<int64_t>(params.point.x - drag_x) * total / canvas.width;
					clamp_view(canvas.width);
				}

				frame->invalidate();
				return 0;

			case pf::mouse_message_type::mouse_leave:
				if (dragging != drag_mode::none) return 0;
				cursor_x = -1;
				cursor_y = -1;
				frame->invalidate();
				return 0;

			case pf::mouse_message_type::left_button_down:
			case pf::mouse_message_type::left_button_dbl_clk:
				drag_x = params.point.x;

				if (map_pane.contains(params.point.y) && canvas.width > 0)
				{
					minimap_seek(params.point.x, canvas.width);
					dragging = drag_mode::minimap;
				}
				else
				{
					dragging = drag_mode::chart;
				}

				drag_start = model.start;
				frame->set_capture();
				frame->invalidate();
				return 0;

			case pf::mouse_message_type::left_button_up:
				dragging = drag_mode::none;
				frame->release_capture();
				// Released outside the window: no mouse_leave follows a captured drag.
				if (params.point.x < 0 || params.point.y < 0 ||
					params.point.x >= canvas.width || params.point.y >= canvas.height)
				{
					cursor_x = -1;
					cursor_y = -1;
					frame->invalidate();
				}
				return 0;

			case pf::mouse_message_type::mouse_wheel:
			{
				if (!model.loaded() || canvas.width <= 0) return 0;

				// Shift turns the wheel into a horizontal scroll, as in most editors.
				if (params.shift)
				{
					model.start += params.wheel_delta > 0 ? -model.span / 8 : model.span / 8;
					clamp_view(canvas.width);
					frame->invalidate();
					return 0;
				}

				const double anchor = std::clamp(static_cast<double>(params.point.x) / canvas.width, 0.0, 1.0);
				const int64_t focus = model.start + static_cast<int64_t>(anchor * model.span);
				const double factor = params.wheel_delta > 0 ? 0.8 : 1.25;

				model.span = static_cast<int64_t>(model.span * factor);
				clamp_view(canvas.width);
				model.start = focus - static_cast<int64_t>(anchor * model.span);
				clamp_view(canvas.width);

				frame->invalidate();
				return 0;
			}

			case pf::mouse_message_type::set_cursor:
				frame->set_cursor_shape(dragging != drag_mode::none
					                        ? pf::cursor_shape::size_we
					                        : pf::cursor_shape::arrow);
				return 1;

			default:
				return 0;
			}
		}

		uint32_t handle_keyboard(pf::window_frame_ptr frame, const pf::keyboard_message_type m,
		                         const pf::keyboard_params& params) override
		{
			if (m != pf::keyboard_message_type::key_down || !model.loaded()) return 0;

			switch (params.vk)
			{
			case pf::platform_key::Home:
				model.start = 0;
				model.span = static_cast<int64_t>(model.series.bars.size());
				break;
			case pf::platform_key::Left:
				model.start -= model.span / 8;
				break;
			case pf::platform_key::Right:
				model.start += model.span / 8;
				break;
			case pf::platform_key::End:
				model.start = static_cast<int64_t>(model.series.bars.size());
				break;
			default:
				return 0;
			}

			clamp_view(canvas.width);
			frame->invalidate();
			return 1;
		}

		void handle_paint(pf::window_frame_ptr& frame, pf::draw_context& dc) override
		{
			const pf::irect client = frame->get_client_rect();
			const int width = std::max(client.width(), 1);
			const int height = std::max(client.height(), 1);

			dpi_scale = frame->get_dpi_scale();
			ensure_font();
			ensure_canvas(width, height);

			render(dc, canvas);
			paints += 1;
		}

		void handle_size(pf::window_frame_ptr& frame, const pf::isize extent, pf::measure_context&) override
		{
			if (extent.cx > 0) clamp_view(extent.cx);
			if (frame) frame->invalidate();
		}

	private:
		static constexpr uint32_t k_offscreen_timer = 1;

		// CW_USEDEFAULT gives a window sized for a text editor; the chart wants
		// 1400x900 logical pixels, centred, and no larger than the screen.
		static pf::window_frame::placement startup_placement()
		{
			const pf::isize screen = pf::platform_screen_size();
			const int width = std::min(scaled(1400), std::max(640, screen.cx - scaled(80)));
			const int height = std::min(scaled(900), std::max(480, screen.cy - scaled(80)));
			const int left = std::max(0, (screen.cx - width) / 2);
			const int top = std::max(0, (screen.cy - height) / 2);

			pf::window_frame::placement p;
			p.normal_bounds = pf::irect(left, top, left + width, top + height);
			return p;
		}
	};
}

app_init_result app_init(const pf::window_frame_ptr& main_frame,
                         const std::span<const std::string_view> params)
{
	app_init_result r;

	offscreen = std::find(params.begin(), params.end(), "--offscreen") != params.end();
	if (offscreen) pf::attach_console();

	load_from_params(params);

	if (main_frame)
	{
		// Paints and lays out for real, but never appears and never takes focus.
		r.offscreen_gui = offscreen;
		main_frame->set_reactor(std::make_shared<viewer_reactor>());
	}

	return r;
}

void app_idle()
{
}

void app_destroy()
{
	if (mono_font)
	{
		pf::delete_font_handle(mono_font);
		mono_font = 0;
	}
}
