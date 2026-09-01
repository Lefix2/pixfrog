// Mobile nav toggle.
//
// Below 820px the centre link group is pulled out of the bar — there is no room
// for it beside the brand and the utilities — and it used to be simply hidden,
// which left Home/Flash/Docs/About unreachable on a phone. It now collapses
// behind this button and drops down as a panel.
//
// The panel is positioned absolutely, so the bar keeps its --nav-h height and
// the full-height hero maths (calc(100svh - var(--nav-h))) is unaffected.
(function () {
  var header = document.querySelector("header.nav");
  var toggle = document.querySelector(".nav-toggle");
  if (!header || !toggle) return;

  function setOpen(open) {
    header.classList.toggle("is-open", open);
    toggle.setAttribute("aria-expanded", open ? "true" : "false");
  }

  toggle.addEventListener("click", function () {
    setOpen(!header.classList.contains("is-open"));
  });

  // Same-page anchors would otherwise leave the panel covering what you jumped to.
  header.querySelectorAll(".nav-links a").forEach(function (a) {
    a.addEventListener("click", function () {
      setOpen(false);
    });
  });

  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") setOpen(false);
  });

  // A panel left open across the breakpoint would sit there as a stray block
  // once the links are back in the bar.
  window.addEventListener("resize", function () {
    if (window.innerWidth > 820) setOpen(false);
  });
})();
