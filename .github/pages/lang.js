(function () {
  function initLang(thisLang, partnerUrl) {
    var saved = localStorage.getItem("lang");
    var browserFr =
      /^fr\b/.test(navigator.language) ||
      (navigator.languages || []).some(function (l) { return /^fr\b/.test(l); });
    var want = saved || (browserFr ? "fr" : "en");
    if (want !== thisLang) location.replace(partnerUrl);
  }

  function switchLang(targetLang, targetUrl) {
    localStorage.setItem("lang", targetLang);
    location.href = targetUrl;
  }

  window.initLang = initLang;
  window.switchLang = switchLang;
})();
