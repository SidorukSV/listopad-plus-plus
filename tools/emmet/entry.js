import expandAbbreviation from 'emmet';

const stylesheetSyntaxes = new Set(['css', 'scss', 'sass', 'less', 'stylus']);

globalThis.listopadExpand = function listopadExpand(abbreviation, syntax) {
  const type = stylesheetSyntaxes.has(syntax) ? 'stylesheet' : 'markup';
  return expandAbbreviation(abbreviation, {
    syntax,
    type,
    options: {
      'output.field': (index, placeholder) => `\${${index}:${placeholder || ''}}`,
      'output.format': true
    }
  });
};

