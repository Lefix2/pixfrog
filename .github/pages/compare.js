// Before/after slider (landing §01): drag — or use the arrow keys — to lift
// the lid off the rack. The open render is clipped to the left of --pos; a
// transparent range input over the image does the dragging, the keyboard and
// the accessibility. Until someone touches it, the handle gives a small
// damped bounce every few seconds while in view, to show it can be moved
// (never with prefers-reduced-motion).
(function () {
  var reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  document.querySelectorAll("[data-compare]").forEach(function (box) {
    var range = box.querySelector("input[type=range]");
    if (!range) return;
    var touched = false;
    var visible = false;
    var frame = 0;

    function set(v) {
      box.style.setProperty("--pos", v + "%");
    }
    function stop() {
      touched = true;
      cancelAnimationFrame(frame);
      box.classList.remove("compare-hint");
    }
    set(range.value);
    range.addEventListener("input", function () {
      stop();
      set(range.value);
    });
    range.addEventListener("pointerdown", stop);
    range.addEventListener("keydown", stop);

    if (reduce || !("IntersectionObserver" in window)) return;
    var timer = 0;
    new IntersectionObserver(
      function (entries) {
        visible = entries[0].isIntersecting;
        // First hint shortly after it scrolls into view, then every few s.
        clearTimeout(timer);
        if (visible && !touched) timer = setTimeout(tick, 1200);
      },
      { threshold: 0.6 }
    ).observe(box);

    // One nudge toward the open side, settling back like a spring.
    function bounce() {
      if (touched || !visible || document.hidden) return;
      var base = +range.value;
      var t0 = performance.now();
      var dur = 1500;
      box.classList.add("compare-hint");
      function step(now) {
        if (touched) return;
        var t = (now - t0) / dur;
        if (t >= 1) {
          set(base);
          box.classList.remove("compare-hint");
          return;
        }
        set(base + 14 * Math.exp(-3.5 * t) * Math.sin(t * Math.PI * 4));
        frame = requestAnimationFrame(step);
      }
      frame = requestAnimationFrame(step);
    }
    function tick() {
      if (touched || !visible) return;
      bounce();
      timer = setTimeout(tick, 5200);
    }
  });
})();
