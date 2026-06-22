(function() {
  'use strict';
  var links = document.querySelectorAll('[data-scrollspy]');
  if (!links.length) return;

  var sections = [];
  links.forEach(function(a) {
    var id = a.getAttribute('data-scrollspy');
    var el = document.getElementById(id);
    if (el) sections.push({ el: el, link: a, id: id });
  });
  if (!sections.length) return;

  var toc = document.getElementById('book-toc');
  if (toc) {
    var observer = new IntersectionObserver(function(entries) {
      entries.forEach(function(entry) {
        var link = document.querySelector('[data-scrollspy="' + entry.target.id + '"]');
        if (link) link.classList.toggle('active', entry.isIntersecting);
      });
    }, { rootMargin: '-80px 0px -60% 0px' });

    sections.forEach(function(s) { observer.observe(s.el); });
  } else {
    function update() {
      var scrollY = window.scrollY + 100;
      var current = sections[0] ? sections[0].id : null;
      for (var i = sections.length - 1; i >= 0; i--) {
        if (sections[i].el.offsetTop <= scrollY) { current = sections[i].id; break; }
      }
      links.forEach(function(a) {
        a.classList.toggle('active', a.getAttribute('data-scrollspy') === current);
      });
    }
    update();
    window.addEventListener('scroll', update, { passive: true });
  }
})();
