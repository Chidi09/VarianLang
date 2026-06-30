(function() {
  'use strict';

  function initTheme() {
    var html = document.documentElement;
    var toggle = document.getElementById("theme-toggle");
    var icon = document.getElementById("theme-icon");
    if (!toggle || !icon) return;

    var theme = localStorage.getItem("theme") || "dark";
    if (theme === "light") {
      html.classList.remove("dark");
      html.classList.add("theme-light");
      icon.textContent = "dark_mode";
    } else {
      html.classList.remove("theme-light");
      html.classList.add("dark");
      icon.textContent = "light_mode";
    }

    toggle.addEventListener("click", function() {
      if (html.classList.contains("dark")) {
        html.classList.remove("dark");
        html.classList.add("theme-light");
        localStorage.setItem("theme", "light");
        icon.textContent = "dark_mode";
      } else {
        html.classList.remove("theme-light");
        html.classList.add("dark");
        localStorage.setItem("theme", "dark");
        icon.textContent = "light_mode";
      }
    });
  }

  function initNavigation() {
    var path = window.location.pathname;
    var cleanPath = path.replace(/\/$/, '').replace(/\.html$/, '');
    if (cleanPath === '') cleanPath = '/';

    // Highlight navbar links
    document.querySelectorAll('.nav-links a').forEach(function(a) {
      var href = a.getAttribute('href');
      if (href) {
        var cleanHref = href.split('#')[0].replace(/\/$/, '').replace(/\.html$/, '');
        if (cleanHref === '') cleanHref = '/';
        if (cleanPath === cleanHref) {
          a.classList.add('active');
          a.style.color = 'var(--accent)';
        }
      }
    });

    // Highlight docs sidebar links
    document.querySelectorAll('.docs-sidebar a').forEach(function(a) {
      var href = a.getAttribute('href');
      if (href) {
        var cleanHref = href.split('#')[0].replace(/\/$/, '').replace(/\.html$/, '');
        if (cleanPath === cleanHref) {
          a.classList.add('active');
        }
      }
    });

    var navToggle = document.getElementById('nav-toggle');
    var navLinks = document.querySelector('.nav-links');
    if (!navToggle || !navLinks) return;

    // Create scrim/overlay dynamically
    var scrim = document.querySelector('.nav-scrim');
    if (!scrim) {
      scrim = document.createElement('div');
      scrim.className = 'nav-scrim';
      document.body.appendChild(scrim);
    }

    function toggleDrawer(forceClose) {
      var isOpen = navLinks.classList.contains('nav-open');
      var shouldOpen = forceClose === undefined ? !isOpen : !forceClose;

      if (shouldOpen) {
        navLinks.classList.add('nav-open');
        scrim.classList.add('scrim-open');
        document.body.classList.add('nav-open-active');
        navToggle.setAttribute('aria-expanded', 'true');
        var iconEl = navToggle.querySelector('.material-symbols-outlined');
        if (iconEl) iconEl.textContent = 'close';
      } else {
        navLinks.classList.remove('nav-open');
        scrim.classList.remove('scrim-open');
        document.body.classList.remove('nav-open-active');
        navToggle.setAttribute('aria-expanded', 'false');
        var iconEl = navToggle.querySelector('.material-symbols-outlined');
        if (iconEl) iconEl.textContent = 'menu';
      }
    }

    navToggle.addEventListener('click', function(e) {
      e.stopPropagation();
      toggleDrawer();
    });

    scrim.addEventListener('click', function() {
      toggleDrawer(true);
    });

    // Close drawer on link tap (for anchor links or page transitions)
    navLinks.querySelectorAll('a').forEach(function(a) {
      a.addEventListener('click', function() {
        toggleDrawer(true);
      });
    });

    // Close drawer on clicking outside
    document.addEventListener('click', function(e) {
      if (navLinks.classList.contains('nav-open') && 
          !navLinks.contains(e.target) && 
          !navToggle.contains(e.target)) {
        toggleDrawer(true);
      }
    });

    // Close on Escape key
    document.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') {
        toggleDrawer(true);
      }
    });
  }

  function initDocsSidebar() {
    var sidebar = document.querySelector('.docs-sidebar');
    if (!sidebar) return;

    var toggleBtn = document.createElement('button');
    toggleBtn.className = 'docs-sidebar-toggle';
    toggleBtn.setAttribute('aria-expanded', 'false');
    toggleBtn.innerHTML = '<span>Docs Navigation</span><span class="material-symbols-outlined">expand_more</span>';

    // Insert at the top of the sidebar
    sidebar.insertBefore(toggleBtn, sidebar.firstChild);

    toggleBtn.addEventListener('click', function() {
      var isExpanded = sidebar.classList.toggle('sidebar-expanded');
      toggleBtn.setAttribute('aria-expanded', isExpanded ? 'true' : 'false');
      toggleBtn.querySelector('.material-symbols-outlined').textContent = isExpanded ? 'expand_less' : 'expand_more';
    });
  }

  function runInit() {
    initTheme();
    initNavigation();
    initDocsSidebar();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', runInit);
  } else {
    runInit();
  }
})();
