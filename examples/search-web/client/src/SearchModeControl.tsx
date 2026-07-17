import type { SearchMode } from "./types";

const options: Array<{ value: SearchMode; label: string; description: string }> = [
  { value: "lexical", label: "キーワード", description: "入力した語句の一致を重視します" },
  { value: "vector", label: "意味検索", description: "言い換えや意味の近い文章を探します" },
  { value: "hybrid", label: "組み合わせ", description: "キーワードと意味検索の順位を統合します" },
];

export function searchModeLabel(mode: SearchMode): string {
  return options.find((option) => option.value === mode)?.label ?? mode;
}

export function SearchModeControl({ mode, availableModes, onChange }: {
  mode: SearchMode;
  availableModes: SearchMode[];
  onChange: (mode: SearchMode) => void;
}) {
  return (
    <fieldset className="search-mode-control">
      <legend>検索方法</legend>
      <div className="search-mode-options">
        {options.map((option) => {
          const available = availableModes.includes(option.value);
          return (
            <label key={option.value} className="search-mode-option">
              <input
                type="radio"
                name="search-mode"
                value={option.value}
                checked={mode === option.value}
                disabled={!available}
                onChange={() => onChange(option.value)}
              />
              <span className="search-mode-name">{option.label}</span>
              <span className="search-mode-description">{option.description}</span>
            </label>
          );
        })}
      </div>
      {availableModes.length === 1 && (
        <p className="field-help">意味検索を使うにはvector indexとembedding serverが必要です。</p>
      )}
    </fieldset>
  );
}
