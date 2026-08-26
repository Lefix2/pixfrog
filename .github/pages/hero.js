(function () {
  // Snap-scrolls the single boundary between the first section and the one
  // after it, the way the design canvas does — and only that boundary. Once
  // you are past the hero the page scrolls normally, so the long reading
  // sections are never hijacked.
  var hero = document.querySelector(".hero");
  if (!hero) return;
  var next = hero.nextElementSibling;
  var chevron = hero.querySelector(".hero-chevron");
  if (!next) return;

  var GLIDE_MS = 900;
  var reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
  var animating = false;
  var armed = 0;
  var touchY = null;
  var watchdog = null;

  function boundary() {
    return Math.round(next.getBoundingClientRect().top + window.scrollY);
  }

  // A hero taller than the viewport has content of its own to scroll
  // through; snapping past it would skip that content.
  function fits() {
    return hero.offsetHeight <= window.innerHeight + 4;
  }

  function engaged() {
    return !reduced.matches && fits();
  }

  function release() {
    animating = false;
    armed = 0;
    if (watchdog) {
      clearTimeout(watchdog);
      watchdog = null;
    }
    paint();
  }

  function glide(to) {
    var from = window.scrollY;
    var dist = to - from;
    if (Math.abs(dist) < 2) return;
    animating = true;
    // Wheel events are swallowed while animating, so a frame loop that
    // never runs (background tab, throttled rAF) must not be able to leave
    // the page unscrollable.
    clearTimeout(watchdog);
    watchdog = setTimeout(release, GLIDE_MS + 300);
    var t0 = performance.now();
    var ease = function (t) {
      return 1 - Math.pow(1 - t, 4);
    };
    (function frame(now) {
      if (!animating) return;
      var t = Math.min(1, (now - t0) / GLIDE_MS);
      // "instant" so html{scroll-behavior:smooth} does not animate on top
      // of this one.
      window.scrollTo({ top: Math.round(from + dist * ease(t)), behavior: "instant" });
      if (t < 1) requestAnimationFrame(frame);
      else release();
    })(performance.now());
  }

  // Returns the target for a step in `dir`, or null when this gesture is
  // not crossing the first boundary and should be left to the browser.
  function step(dir) {
    var edge = boundary();
    var y = window.scrollY;
    if (dir > 0 && y < edge - 4) return edge;
    if (dir < 0 && y > 0 && y <= edge + 4) return 0;
    return null;
  }

  function drive(dir) {
    if (!engaged()) return false;
    var to = step(dir);
    if (to === null) return false;
    var now = Date.now();
    if (now - armed < 90) return true;
    armed = now;
    glide(to);
    return true;
  }

  function onWheel(e) {
    if (Math.abs(e.deltaY) < 2) return;
    if (animating) {
      e.preventDefault();
      return;
    }
    if (!engaged()) return;
    if (step(e.deltaY > 0 ? 1 : -1) === null) return;
    e.preventDefault();
    drive(e.deltaY > 0 ? 1 : -1);
  }

  function onTouchStart(e) {
    touchY = e.touches[0].clientY;
  }

  function onTouchEnd(e) {
    if (touchY === null || animating) return;
    var dy = touchY - e.changedTouches[0].clientY;
    touchY = null;
    if (Math.abs(dy) > 44) drive(dy > 0 ? 1 : -1);
  }

  function paint() {
    if (!chevron) return;
    // Fade out as soon as the hero starts leaving, so it never lingers
    // over the section below.
    var gone = window.scrollY > Math.max(80, hero.offsetHeight * 0.3);
    chevron.classList.toggle("is-gone", gone);
  }

  window.addEventListener("wheel", onWheel, { passive: false });
  window.addEventListener("touchstart", onTouchStart, { passive: true });
  window.addEventListener("touchend", onTouchEnd, { passive: true });
  window.addEventListener("scroll", function () {
    if (!animating) paint();
  }, { passive: true });

  if (chevron) {
    chevron.addEventListener("click", function (e) {
      // The href is a real anchor, so this still works without the glide.
      if (reduced.matches) return;
      e.preventDefault();
      if (!animating) glide(boundary());
    });
  }

  paint();
})();
