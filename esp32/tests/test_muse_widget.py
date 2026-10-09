# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""A reply's widgets (muse_widget.c): the wire's records and presentations,
as captured on a device, into the face's model. Kinds are matched loosely,
one that can't show as its own kind becomes a card, an image is none. What
a tap sends is the words as they came; what's shown has ASCII stand-ins and
is cut at a whole character, with "...", never past its buffer."""
from pathlib import Path
import json
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    'CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'
))

# delta.message_done's "widgets" entry and a delta.presentation's payload, as captured.
OPTION = {
    "data": {"button_style": "dotted_lines", "options": ["Pizza", "Tacos", "Sushi"]},
    "display_text": "Pizza / Tacos / Sushi", "kind": "option",
    "state": {"data": {}, "version": 0},
    "widget_id": "widget-31f0904a-89f7-4071-a169-8006fd06d4fe",
}
GENERIC_LIST = {
    "agent_id": "b3855dd2-b53e-49f0-b7f2-7ab232fe7545",
    "data": {"items": [
        {"data": {"text": "Let's hike Rattlesnake Ledge this weekend"}, "icons": [],
         "subtitle": "Tap to reply with this plan", "title": "Hike Rattlesnake Ledge", "type": "send_message"},
        {"data": {"url": "https://www.pikeplacemarket.org/"}, "icons": [],
         "subtitle": "Opens the market site", "title": "Pike Place Market", "type": "link"},
        {"data": {}, "icons": [], "subtitle": "Glyph-only row", "title": "Block Saturday morning",
         "type": "calendar"}],
        "title": "Weekend ideas"},
    "display_text": "**Weekend ideas**\n- Hike Rattlesnake Ledge: Tap to reply with this plan",
    "id": "widget-d2e24d2e-4bbb-49b3-9530-4204c2738938", "kind": "generic_list",
    "session_id": "4c25a203-0a88-4782-8aec-5792fd878218", "state": {"data": {}, "version": 0},
}
MAP = {
    "id": "widget-map", "kind": "map",
    "data": {"display_text": "Coffee", "elements": [
        {"kind": "rich_place", "place_id": "1", "name": "Victrola", "category": "Coffee shop",
         "coordinate": {"latitude": 47.6, "longitude": -122.3}},
        {"kind": "marker", "title": "Home", "coordinate": {"latitude": 47.6, "longitude": -122.3}},
        {"kind": "rich_place", "place_id": "2", "coordinate": {"latitude": 0, "longitude": 0}}]},
}


def valid_utf8(s):
    return s.encode('utf-8').decode('utf-8') == s


class MuseWidgetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_widget"
        cc = shlex.split(os.environ.get("CC", "cc"))
        compiled = subprocess.run(
            [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-Wno-deprecated-declarations",   # cJSON.c's sprintf, on macOS
             "-I", str(ROOT / "components/muse"), "-I", str(JSON),
             str(ROOT / "tests/muse_widget_harness.c"), str(ROOT / "components/muse/muse_widget.c"),
             str(ROOT / "components/muse/muse_widget_html.c"), str(ROOT / "components/muse/muse_browse.c"),
             str(ROOT / "components/muse/muse_map.c"),
             str(ROOT / "components/muse/muse_text.c"), str(JSON / "cJSON.c"), "-lm",
             "-o", str(cls.binary)],
            capture_output=True, text=True,
        )
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_harness(self, *args):
        env = {**os.environ,
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        ran = subprocess.run([str(self.binary), *args], capture_output=True, env=env)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode())
        return ran.stdout.decode('utf-8')

    def parse(self, obj):
        return json.loads(self.run_harness("parse", obj if isinstance(obj, str) else json.dumps(obj)))

    def text(self, cap, src, lines=False):
        out = self.run_harness("text", str(cap), "1" if lines else "0", src)
        self.assertLess(len(out.encode('utf-8')), cap)
        self.assertTrue(valid_utf8(out))
        return out

    def test_options_send_their_exact_text(self):
        w = self.parse(OPTION)
        self.assertEqual((w["kind"], w["name"], w["id"], w["filled"]),
                         ("options", "option", OPTION["widget_id"], False))
        self.assertEqual([(r["type"], r["title"], r["send"]) for r in w["rows"]],
                         [("option", t, t) for t in ("Pizza", "Tacos", "Sushi")])
        filled = dict(OPTION, kind="options", data={"button_style": "center_aligned_filled",
                                                    "options": ["Just today’s", {"label": "Every week"}]})
        w = self.parse(filled)
        self.assertTrue(w["filled"])
        self.assertEqual([(r["title"], r["send"]) for r in w["rows"]],
                         [("Just today's", "Just today’s"), ("Every week", "Every week")])

    def test_generic_list_rows(self):
        w = self.parse(GENERIC_LIST)
        self.assertEqual((w["kind"], w["title"], w["id"]), ("list", "Weekend ideas", GENERIC_LIST["id"]))
        rows = w["rows"]
        self.assertEqual([r["type"] for r in rows], ["send", "link", "calendar"])
        self.assertEqual(rows[0]["send"], "Let's hike Rattlesnake Ledge this weekend")
        # Every row does something: a link's card asks Muse to sum it up, the rest ask about them.
        self.assertEqual((rows[1]["meta"], rows[1]["url"], rows[1]["send"], rows[1]["send2"]),
                         ("pikeplacemarket.org", "https://www.pikeplacemarket.org/",
                          "Summarise https://www.pikeplacemarket.org/ for me", "Tell me more about Pike Place Market"))
        self.assertEqual((rows[2]["title"], rows[2]["sub"], rows[2]["button"], rows[2]["send"]),
                         ("Block Saturday morning", "Glyph-only row", "Tell me more",
                          "Tell me more about Block Saturday morning"))
        # The tool's own name for it, and its record's data_json string.
        record = {"widget_id": "w-2", "kind": "list", "data_json": json.dumps(GENERIC_LIST["data"])}
        self.assertEqual(len(self.parse(record)["rows"]), 3)

    def test_flight_row_answers_with_its_itinerary(self):
        data = json.loads(self.run_harness("sample", "list"))
        flight = data["rows"][-1]
        self.assertEqual((flight["type"], flight["title"], flight["meta"], flight["button"]),
                         ("flight", "SEA -> SFO", "$189", "Book"))
        self.assertEqual(flight["sub"], "Alaska AS330, Nonstop")
        self.assertEqual(flight["extra"], "Oct 12, 8:05 AM - 10:20 AM")
        self.assertTrue(flight["send"].startswith("Book this flight: SEA to SFO, Alaska AS330"))
        self.assertIn("2026-10-12T08:05", flight["send"])

    def test_map_places(self):
        w = self.parse(MAP)
        self.assertEqual((w["kind"], w["title"]), ("map", "Coffee"))   # its display_text, a short line
        self.assertEqual([(r["type"], r["title"], r["sub"]) for r in w["rows"]],
                         [("place", "Victrola", "Coffee shop"), ("place", "Home", "")])   # one with no name: none
        self.assertEqual(w["rows"][0]["send"], "Tell me more about Victrola")

    def test_shopping_products(self):
        w = json.loads(self.run_harness("sample", "shopping"))
        self.assertEqual(w["kind"], "shopping")
        shoes, socks = w["rows"]
        self.assertEqual((shoes["meta"], shoes["extra"], shoes["button"], shoes["sub"]),
                         ("$99.99", "$129.99", "Add to cart", "REI"))
        self.assertEqual(shoes["send"], "Add one more Trail running shoes to my cart")
        self.assertEqual(shoes["image"], "https://www.rei.com/media/product/2286170001")
        self.assertEqual(shoes["send2"], "Tell me more about the Trail running shoes")
        # One that can't go in the cart: its card asks about it, or for more like it.
        self.assertEqual((socks["button"], socks["send"], socks["meta"], socks["button2"]),
                         ("Tell me more", "Tell me more about the Merino wool socks, 3 pack from Amazon", "$24.00",
                          "Find similar"))
        # A cart with nothing to list: a card.
        self.assertEqual(self.parse({"kind": "shopping_cart", "data": {"cart_ref": "c1"}})["kind"], "card")

    def test_cards_and_unknown_kinds(self):
        # An HTML page with nothing to answer here: a card, with its fallback words.
        w = self.parse({"id": "x", "kind": "html", "display_text": "Packing list",
                        "data": {"html": "<b>hi</b>", "fallback_text": "12 things, 4 packed"}})
        self.assertEqual((w["kind"], w["title"], w["text"], w["rows"]), ("card", "Packing list", "12 things, 4 packed", []))
        w = self.parse({"kind": "letter", "data": {"content": {"type": "briefing", "data": {"id": "b"}}}})
        self.assertEqual((w["kind"], w["title"]), ("card", "Briefing"))
        w = self.parse({"kind": "navigation", "data": {"title": "Open Health", "summary": "Your sleep page",
                                                       "ui_control": {"action": "navigate", "target": "health"}}})
        self.assertEqual((w["title"], w["text"]), ("Open Health", "Your sleep page"))
        w = self.parse({"kind": "brand_new_thing", "display_text": "**Heads up**\n- one\n- two"})
        self.assertEqual((w["kind"], w["name"], w["title"], w["text"]),
                         ("card", "brand_new_thing", "Heads up", "• one\n• two"))
        # Options with none to pick: a card with what it says.
        w = self.parse({"kind": "option", "display_text": "Nothing to pick", "data": {"options": []}})
        self.assertEqual((w["kind"], w["title"]), ("card", "Nothing to pick"))

    def test_not_widgets(self):
        self.assertIsNone(self.parse({"kind": "image", "data": {"images": []}}))
        self.assertIsNone(self.parse({"state": {}}))
        self.assertIsNone(self.parse("[1, 2]"))

    def test_caps(self):
        many = dict(OPTION, data={"options": [f"Option {i}" for i in range(30)]})
        self.assertEqual(len(self.parse(many)["rows"]), 20)
        rows = [{"type": "generic", "title": f"Row {i}"} for i in range(25)]
        self.assertEqual(len(self.parse({"kind": "generic_list", "data": {"items": rows}})["rows"]), 20)

    def test_long_text_is_cut_at_a_whole_character(self):
        long = "été " * 200   # accented: stand-ins
        w = self.parse({"kind": "generic_list", "data": {"title": long, "items": [{"title": long}]}})
        for s in (w["title"], w["rows"][0]["title"]):
            self.assertTrue(s.endswith("...") and valid_utf8(s) and "é" not in s, s)
        # Characters with no stand-in stay whole: cut before one rather than through it.
        for cap in range(8, 40):
            out = self.text(cap, "你好" * 30)
            self.assertTrue(out.endswith("..."), (cap, out))
        # An option's send is the words as they came, cut whole, never with "...".
        cjk = "你" * 120   # 360 bytes
        w = self.parse(dict(OPTION, data={"options": [cjk]}))
        send = w["rows"][0]["send"]
        self.assertTrue(valid_utf8(send) and cjk.startswith(send) and len(send.encode()) <= 239)

    def test_text_for_the_face(self):
        self.assertEqual(self.text(64, "  **Bold**  and\n\n`code`  "), "Bold and code")
        self.assertEqual(self.text(64, "# Title\n\n\n\n- a\n* b", True), "Title\n\n• a\n• b")
        self.assertEqual(self.text(64, "“Hi” — there"), '"Hi" -- there')
        self.assertEqual(self.text(20, "one two three four five six"), "one two three...")
        self.assertEqual(self.text(4, "abcdef"), "abc")
        self.assertEqual(self.text(1, "abc"), "")

    def test_every_sample_parses(self):
        for name in ("option", "options", "list", "map", "localmap", "shopping", "text", "multi", "card"):
            w = json.loads(self.run_harness("sample", name))
            self.assertTrue(w["rows"] or w["kind"] == "card", name)
            for r in w["rows"]:
                self.assertTrue(r["title"] or r["type"] == "field", name)

    # ---- HTML widgets: read for their controls (muse_widget_html.c) ----

    def fixture(self, name):
        return json.loads((ROOT / 'tests/widget_fixtures' / name).read_text())

    def test_html_text_input_is_a_form(self):
        # Captured: "What's your favorite book?", a text input with a placeholder, a Submit button.
        w = self.parse(self.fixture('html_text.json'))
        self.assertEqual((w["kind"], w["title"], w["action"]), ("form", "What's your favorite book?", "Submit"))
        self.assertEqual([(r["type"], r["title"], r["sub"]) for r in w["rows"]], [("field", "", "Type the title...")])

    def test_html_multi_select_from_its_script(self):
        # Captured: the choices are made by the page's script from a list of words, as checkboxes.
        w = self.parse(self.fixture('html_multi.json'))
        self.assertEqual((w["kind"], w["title"], w["action"], w["lead"]),
                         ("checks", "Pick your pizza toppings (as many as you want):", "Done", "Pizza toppings"))
        self.assertEqual([r["send"] for r in w["rows"]],
                         ["Pepperoni", "Sausage", "Mushrooms", "Bell peppers", "Onions", "Olives", "Extra cheese",
                          "Pineapple", "Jalapeños", "Basil"])
        self.assertEqual(w["rows"][8]["title"], "Jalapenos")   # shown in the face's characters
        self.assertTrue(all(r["type"] == "check" for r in w["rows"]))

    def page(self, html, **data):
        return self.parse({"id": "h", "kind": "html", "fallback_text": "Fallback", "data": {"html": html, **data}})

    def test_html_static_controls(self):
        # Radios with their labels: one-of options, each sending its words.
        w = self.page('<h3>Crust?</h3><label><input type="radio" name="c" value="thin"> Thin &amp; crispy</label>'
                      '<label><input type=radio name=c> Deep dish</label><button>OK</button>')
        self.assertEqual((w["kind"], w["title"]), ("options", "Crust?"))
        self.assertEqual([(r["type"], r["send"]) for r in w["rows"]],
                         [("option", "Thin & crispy"), ("option", "Deep dish")])
        # A select's options.
        w = self.page('<p>Size</p><select id=s><option value=1>Small</option><option>Large</option></select>')
        self.assertEqual([r["send"] for r in w["rows"]], ["Small", "Large"])
        # Static checkboxes; the button names the answer's button.
        w = self.page('<div>Choose your sides:</div><input type=checkbox id=a><label for=a>Fries</label>'
                      '<input type="checkbox" value="Salad"><input type=submit value="Order">')
        self.assertEqual((w["kind"], w["action"], w["lead"]), ("checks", "Order", "Sides"))
        self.assertEqual([r["send"] for r in w["rows"]], ["Fries", "Salad"])
        # Several fields, each with its label; a textarea too.
        w = self.page('<div>Tell me about the trip</div><label for=d>Destination</label><input id=d type=text>'
                      '<span>Dates</span><input type=date><textarea placeholder="Anything else?"></textarea>'
                      '<button>Plan it</button>')
        self.assertEqual((w["kind"], w["title"], w["action"]), ("form", "Tell me about the trip", "Plan it"))
        self.assertEqual([(r["title"], r["sub"]) for r in w["rows"]],
                         [("Destination", ""), ("Dates", ""), ("", "Anything else?")])
        # Buttons made by the script from objects with labels: options.
        w = self.page('<div>Which one?</div><div id=b></div><script>var opts=[{id:1,label:"Red"},{id:2,label:"Blue"}];'
                      'opts.forEach(function(o){var b=document.createElement("button");})</script>')
        self.assertEqual((w["kind"], [r["send"] for r in w["rows"]]), ("options", ["Red", "Blue"]))
        # Nothing to answer: a card with its fallback.
        w = self.page('<div>Just a chart</div><canvas></canvas><script>var data=[1,2,3];</script>')
        self.assertEqual((w["kind"], w["text"]), ("card", "Fallback"))

    def test_lead_in(self):
        for prompt, lead in (("Pick your pizza toppings (as many as you want):", "Pizza toppings"),
                             ("Select the days you're free", "Days you're free"),
                             ("Choose any snacks you like", "Snacks"),
                             ("Which toppings do you want?", ""), ("Toppings:", "")):
            self.assertEqual(self.run_harness("lead", prompt), lead, prompt)

    # ---- Maps ----

    def test_local_map(self):
        # Captured: local_map's places are in its typed_data, its zoom in its page.
        w = self.parse(self.fixture('local_map.json'))
        self.assertEqual((w["kind"], w["title"], w["zoom"]), ("map", "Coffee shops near Pike Place Market", 16))
        first = w["rows"][0]
        self.assertEqual((first["type"], first["title"], first["sub"]),
                         ("place", "Storyville Coffee", "94 Pike St -- cozy, bay views"))
        self.assertAlmostEqual(first["lat"], 47.608882, places=4)
        self.assertAlmostEqual(first["lon"], -122.340398, places=4)
        self.assertEqual((first["button"], first["send"], first["button2"], first["send2"]),
                         ("Tell me more", "Tell me more about Storyville Coffee", "Directions",
                          "How do I get to Storyville Coffee?"))
        self.assertEqual(len(w["rows"]), 4)

    def test_map_arithmetic(self):
        # Pike Place to Capitol Hill: about a mile and a quarter north-east; both fit at a street zoom.
        m = json.loads(self.run_harness("map", "47.6097", "-122.3422", "47.6205", "-122.3212", "1"))
        self.assertEqual((m["distance"], m["compass"]), ("1.2 mi", "NE"))
        self.assertGreaterEqual(m["zoom"], 13)
        m = json.loads(self.run_harness("map", "47.6097", "-122.3422", "47.6100", "-122.3422", "0"))
        self.assertEqual((m["distance"], m["compass"], m["zoom"]), ("30 m", "N", 17))
        m = json.loads(self.run_harness("map", "47.6", "-122.3", "45.5", "-122.7", "0"))
        self.assertEqual((m["distance"], m["compass"]), ("236 km", "S"))

    # ---- Muse's browser (muse_browse.c) ----

    def browse(self, *names, turn=None):
        steps = []
        for n in names:
            steps += self.fixture(n)
        path = Path(self.temp.name) / 'browse.json'
        path.write_text(json.dumps(steps))
        return json.loads(self.run_harness("browse", str(path), *([str(turn)] if turn is not None else [])))

    def test_browser_task_history(self):
        # Captured: 23 updates of one task, one step a page, what it's at the latest.
        b = self.browse('browser_q5.json')
        self.assertEqual((b["task"], b["done"]), ("browser-task:a269152d-ad5f-42ad-9f2c-37a308a77bfe", False))
        self.assertEqual([(s["what"], s["site"], s["title"]) for s in b["steps"]], [
            ("Checking site", "rei.com", "404 - Page Not Found | REI Co-op"),
            ("Selecting option", "rei.com", "mens trail running shoes at REI | REI Co-op"),
            ("Checking site", "rei.com", "Salomon Aero Glide 4 GRVL Trail-Running Shoes - Men's | REI Co-op"),
            ("Opened page", "rei.com", "mens trail running shoes at REI | REI Co-op")])
        # An older, finished task replayed among them (bestbuy) is passed over.
        self.assertFalse(any("bestbuy" in s["site"] for s in b["steps"]))

    def test_browser_task_of_the_turn(self):
        # Captured: the next turn's updates have the last turn's task still going, and an old finished one;
        # only the task the turn started is its own.
        b = self.browse('browser_q5.json', 'browser_q6.json', turn=23)
        self.assertEqual(b["task"], "browser-task:3cadd6fd-4a23-434b-8e81-6fcf875862d5")
        self.assertEqual([(s["what"], s["site"]) for s in b["steps"]], [("Scrolling page", "spl.org")])
        self.assertEqual(b["steps"][0]["title"], "Central Library | The Seattle Public Library")

    def test_browser_task_done(self):
        start = {"kind": "browser_task", "data": {"browser_task_id": "t1", "status": "Queued",
                                                  "activity_title": "Starting browser..."}}
        page = {"kind": "browser_task", "data": {"browser_task_id": "t1", "status": "Running",
                                                 "current_site": {"domain": "www.spl.org", "page_title": "SPL",
                                                                  "url": "https://www.spl.org/"}}}
        done = {"kind": "browser_task", "data": {"browser_task_id": "t1", "status": "Completed",
                                                 "activity_title": "Task completed", "display_state": "completed",
                                                 "current_site": page["data"]["current_site"]}}
        path = Path(self.temp.name) / 'done.json'
        path.write_text(json.dumps([start, page, page, done]))
        b = json.loads(self.run_harness("browse", str(path)))
        self.assertEqual((b["done"], b["failed"]), (True, False))
        self.assertEqual([(s["what"], s["site"], s["at"]) for s in b["steps"]], [("Task completed", "spl.org", 0)])


if __name__ == "__main__":
    unittest.main()
