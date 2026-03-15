import { getLocalesUrl, useI18n } from '../../../i18n.js';

export default ({mobile = false}) => {
  const { locale, setLocalePreference, availableLocales } = useI18n();

  const images = Object.fromEntries(availableLocales.map((item) => {
    return [item.code, (menu = false) => {
      return <>
        <img width="40" height="22" class="p-0 ml-3" src={getLocalesUrl(item.code + '.png')} alt={`${item.code.toUpperCase()} (${item.nativeName})`}/>
        <span
          style={menu ? {whiteSpace: 'nowrap'} : {}}
          class={menu ? 'ml-1' : 'ml-3 mr-3'}
        >{item.code.toUpperCase() + (menu ? ` (${item.nativeName})` : '')}</span>
      </>;
    }];
  }));

  const baseClasses = "no-underline rounded transition-colors";
  const activeClass = 'text-[hsl(var(--card-foreground))] hover:bg-[hsl(var(--primary)/0.8)] hover:text-[hsl(var(--primary-foreground))]';

  const DesktopWrapper = mobile ? ({children}) => <>{children}</> : ({children}) => <li class="mx-1">{children}</li>

  return (
    <DesktopWrapper>
      <div
        class={`dropdown ${baseClasses} ${activeClass} ${mobile ? ' w-full ' : ''}`}
      >
        <div tabindex="0" class={`flex cursor-pointer border-0 font-medium items-center ${mobile ? 'w-full px-4 py-3' : 'py-1.5'}`}>
          {images[locale]()}
        </div>
        <ul
          tabindex="-1"
          class="rounded transition-colors dropdown-content menu bg-[hsl(var(--card))]"
          style="width: max-content; border: 1px solid hsl(var(--primary)/0.8);"
        >
          {Object.keys(images).map((code) => (
            <li
              style="flex-flow: row nowrap;"
              class={`flex cursor-pointer border-0 font-medium items-center cursor-pointer ${baseClasses} py-1 ${activeClass}`}
              onClick={(e) => {
                setLocalePreference(code);
                document.activeElement.blur();
              }}
            >
              {images[code](true)}
            </li>
          ))}
        </ul>
      </div>
    </DesktopWrapper>
  );
};

