(function () {
  // Single source of truth for "which language does this visitor want?":
  // an explicit prior choice (localStorage) wins, else sniff the browser.
  function preferredLang() {
    var saved = localStorage.getItem("lang");
    if (saved) return saved;
    var browserFr =
      /^fr\b/.test(navigator.language) ||
      (navigator.languages || []).some(function (l) { return /^fr\b/.test(l); });
    return browserFr ? "fr" : "en";
  }

  // Called from a content page: bounce to the partner page if the visitor's
  // preferred language differs from the one this page is written in.
  function initLang(thisLang, partnerUrl) {
    if (preferredLang() !== thisLang) location.replace(partnerUrl);
  }

  // Called from the language toggle: remember the choice, then navigate.
  function switchLang(targetLang, targetUrl) {
    localStorage.setItem("lang", targetLang);
    location.href = targetUrl;
  }

  // Called from the root index.html router: send the visitor to the EN or FR
  // landing page based on their preferred language.
  function routeLang(enUrl, frUrl) {
    location.replace(preferredLang() === "fr" ? frUrl : enUrl);
  }

  window.initLang = initLang;
  window.switchLang = switchLang;
  window.routeLang = routeLang;
})();
