export function createCharacterRail({ list, previous, next }) {
  let activeCharacter = null, viewportWidth = 0;
  function updateButtons() {
    const maximum = Math.max(0, list.scrollWidth - list.clientWidth), fits = maximum <= 1;
    previous.hidden = next.hidden = fits;
    previous.disabled = fits || list.scrollLeft <= 1;
    next.disabled = fits || list.scrollLeft >= maximum - 1;
    list.style.setProperty("--roster-fade-start", previous.disabled ? "0px" : "24px");
    list.style.setProperty("--roster-fade-end", next.disabled ? "0px" : "24px");
  }
  function scroll(direction) {
    list.scrollBy({ left: direction * list.clientWidth * 0.75,
      behavior: window.matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth" });
  }
  previous.onclick = () => scroll(-1);
  next.onclick = () => scroll(1);
  list.addEventListener("wheel", (event) => {
    if (Math.abs(event.deltaY) > Math.abs(event.deltaX) && list.scrollWidth > list.clientWidth) {
      event.preventDefault();
      list.scrollLeft += event.deltaY;
    }
  }, { passive: false });
  list.addEventListener("scroll", updateButtons, { passive: true });
  const resize = new ResizeObserver(() => requestAnimationFrame(refreshRoster));
  resize.observe(list);
  function refreshRoster() {
    const resized = viewportWidth !== list.clientWidth;
    viewportWidth = list.clientWidth;
    const active = list.clientWidth ? list.querySelector(".character-choice.active") : null;
    if (active && (active !== activeCharacter || resized)) {
      const viewport = list.getBoundingClientRect(), target = active.getBoundingClientRect();
      const offset = target.left < viewport.left ? target.left - viewport.left : target.right > viewport.right ? target.right - viewport.right : 0;
      if (offset) list.scrollBy({ left: offset, behavior: "auto" });
    }
    activeCharacter = active;
    updateButtons();
  }
  refreshRoster();
  return { refreshRoster };
}
