let nextErrorId = 0;

export function createNumberValidation(form, t, selector = 'input[type="number"][min][max]') {
  const messages = new WeakMap();
  const format = (value) => Number(value).toLocaleString(document.documentElement.lang || "en", { maximumFractionDigits: 12 });
  function update(input) {
    const valid = !input.willValidate || input.validity.valid;
    let message = messages.get(input);
    if (valid) {
      input.removeAttribute("aria-invalid");
      if (message) { message.textContent = ""; message.hidden = true; }
      return;
    }
    if (!message) {
      const label = input.closest("label");
      if (label && !input.hasAttribute("aria-label") && !input.hasAttribute("aria-labelledby")) input.setAttribute("aria-label", label.textContent.trim());
      message = document.createElement("small");
      message.className = "field-error";
      message.id = `number-error-${++nextErrorId}`;
      message.setAttribute("role", "status");
      input.insertAdjacentElement("afterend", message);
      input.setAttribute("aria-describedby", [input.getAttribute("aria-describedby"), message.id].filter(Boolean).join(" "));
      messages.set(input, message);
    }
    input.setAttribute("aria-invalid", "true");
    message.textContent = input.validity.customError ? input.validationMessage :
      t(input.step === "1" ? "web.number_integer_range" : input.step === "any" ? "web.number_range" : "web.number_step_range",
        format(input.min), format(input.max), format(input.step || "1"));
    message.hidden = false;
  }
  form.addEventListener("input", (event) => { if (event.target.matches(selector)) update(event.target); });
  form.addEventListener("invalid", (event) => {
    if (!event.target.matches(selector)) return;
    for (let details = event.target.closest("details"); details; details = details.parentElement?.closest("details")) details.open = true;
    update(event.target);
  }, true);
  return () => { for (const input of form.querySelectorAll(selector)) update(input); };
}
