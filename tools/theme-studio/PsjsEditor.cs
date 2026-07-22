using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Xml;
using ICSharpCode.AvalonEdit;
using ICSharpCode.AvalonEdit.CodeCompletion;
using ICSharpCode.AvalonEdit.Document;
using ICSharpCode.AvalonEdit.Editing;
using ICSharpCode.AvalonEdit.Highlighting;
using ICSharpCode.AvalonEdit.Highlighting.Xshd;

namespace ThemeStudio
{
   // the script box: colours from psjs.xshd, and a list of names that offers itself when there is
   // exactly one right answer to offer. PSJS has no libraries and no user types, so every name a
   // script can use is known in advance -- which is what makes suggesting them worth doing at all.
   public class PsjsEditor
   {
      private readonly TextEditor editor;
      private readonly Func<IEnumerable<string>> getSceneNames;

      public PsjsEditor(TextEditor editor, Func<IEnumerable<string>> getSceneNames)
      {
         this.editor = editor;
         this.getSceneNames = getSceneNames;
         editor.SyntaxHighlighting = loadColours();
         editor.TextArea.TextEntered += onTextEntered;
         // the square where the two scrollbars meet is painted from the system palette, which is
         // white. it is the only part of the editor that does not read its own colours.
         editor.Resources[SystemColors.ControlBrushKey] = editor.Background;
      }

      private static IHighlightingDefinition loadColours()
      {
         using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("ThemeStudio.psjs.xshd"))
         using (var reader = new XmlTextReader(stream))
            return HighlightingLoader.Load(reader, HighlightingManager.Instance);
      }

      // a dot or an arrow narrows what can legally come next to one short list, so that list opens
      // by itself. an ordinary letter does not, so nothing appears until Ctrl+Space asks for it.
      private void onTextEntered(object sender, TextCompositionEventArgs e)
      {
         if (e.Text == ".") {
            string[] members = getMembersAfter(getWordBefore(editor.CaretOffset - 1));
            if (members != null) showPopup(members, editor.CaretOffset);
            return;
         }
         if (e.Text == ">" && isArrowBefore()) showPopup(PsjsSnippets.PartNames, editor.CaretOffset);
      }

      // what the thing before the dot actually offers, or null when there is nothing certain to
      // say -- a list of everything would claim a camera can be scaled and a light rotated.
      private string[] getMembersAfter(string word)
      {
         if (word == "Math") return PsjsSnippets.MathNames;
         if (word == "System") return PsjsSnippets.SystemMembers;
         return PsjsSnippets.GetMembersOf(findDeclaredType(word));
      }

      // a script says what its own names are: "var thing = new Actor(...)". nothing else can be
      // asked -- the language has no types of its own beyond these.
      private string findDeclaredType(string name)
      {
         if (name.Length == 0) return "";
         Match found = Regex.Match(editor.Text, @"\bvar\s+" + Regex.Escape(name) + @"\s*=\s*new\s+(\w+)\s*\(");
         return found.Success ? found.Groups[1].Value : "";
      }

      private bool isArrowBefore()
      {
         int offset = editor.CaretOffset - 2;   // the > has been typed; the - is the character before it
         return offset >= 0 && editor.Document.GetCharAt(offset) == '-';
      }

      // Ctrl+Space: everything a script can name here, filtered by whatever has been typed so far
      public void ShowSuggestions()
      {
         showPopup(getSceneNames(), findWordStart(editor.CaretOffset));
      }

      private void showPopup(IEnumerable<string> names, int startOffset)
      {
         // wide enough for the longest hint; the default is sized for bare names and cuts them off.
         // no window chrome either: it arrives as an ordinary window, complete with a white title
         // bar you can drag the suggestions around by.
         var popup = new CompletionWindow(editor.TextArea) {
            StartOffset = startOffset, Width = 560,
            WindowStyle = WindowStyle.None, ResizeMode = ResizeMode.NoResize,
            BorderThickness = new Thickness(1)   // its own edge, now that the chrome is gone
         };
         foreach (string name in names) popup.CompletionList.CompletionData.Add(new SuggestedName(name));
         popup.Show();   // it keeps itself alive while it is up, and closes itself when it is done
         wearEditorColours(popup);
      }

      // the popup is a window of its own, so it is born in the system's own colours -- white, over
      // a dark editor. these are the editor's.
      private static readonly Brush PopupBackground = new SolidColorBrush(Color.FromRgb(0x25, 0x25, 0x26));
      private static readonly Brush PopupForeground = new SolidColorBrush(Color.FromRgb(0xDC, 0xDC, 0xDC));
      private static readonly Brush PopupBorder = new SolidColorBrush(Color.FromRgb(0x3F, 0x3F, 0x3F));
      private static readonly Brush PopupHint = new SolidColorBrush(Color.FromRgb(0x8A, 0x8A, 0x8A));

      // the popup builds its own list from a template of AvalonEdit's, which is white and does not
      // read either the window's colours or a style put in its resources -- so the list is found
      // once it exists and coloured directly. must run after Show, or there is nothing to find.
      private static void wearEditorColours(CompletionWindow popup)
      {
         popup.Background = PopupBackground;
         popup.BorderBrush = PopupBorder;
         popup.UpdateLayout();
         paintLists(popup);
      }

      private static void paintLists(DependencyObject parent)
      {
         for (int index = 0; index < VisualTreeHelper.GetChildrenCount(parent); index++) {
            DependencyObject child = VisualTreeHelper.GetChild(parent, index);
            var list = child as ListBox;
            if (list != null) {
               list.Background = PopupBackground;
               list.Foreground = PopupForeground;
               list.BorderThickness = new Thickness(0);
               list.Padding = new Thickness(0, 4, 0, 4);   // the list stops short of its own edge
               list.ItemTemplate = makeRowTemplate();
            }
            paintLists(child);
         }
      }

      // AvalonEdit's own row keeps a column for an icon, which these names do not have -- so every
      // one of them starts a long way in from the edge. this row is the name and nothing else.
      private static DataTemplate makeRowTemplate()
      {
         var row = new FrameworkElementFactory(typeof(ContentPresenter));
         row.SetBinding(ContentPresenter.ContentProperty, new System.Windows.Data.Binding("Content"));
         row.SetValue(FrameworkElement.MarginProperty, new Thickness(8, 2, 8, 2));
         return new DataTemplate { VisualTree = row };
      }

      // the popup starts at the beginning of the word, so what is already typed narrows the list
      // instead of being typed twice
      private int findWordStart(int offset)
      {
         while (offset > 0 && isNameCharacter(editor.Document.GetCharAt(offset - 1))) offset--;
         return offset;
      }

      // the word ending just before the given offset, which is how "Math." is told from "thing."
      private string getWordBefore(int offset)
      {
         int start = findWordStart(offset);
         return editor.Document.GetText(start, offset - start);
      }

      private static bool isNameCharacter(char character)
      {
         return char.IsLetterOrDigit(character) || character == '_';
      }

      // one row of the popup: the name, and beside it how it is called or what it holds. the hint
      // is part of the row rather than a tooltip, so it is there while the list is being read.
      private class SuggestedName : ICompletionData
      {
         public SuggestedName(string text) { Text = text; }

         public string Text { get; private set; }
         public object Description { get { return null; } }

         public object Content
         {
            get
            {
               string hint = PsjsSnippets.GetHint(Text);
               if (hint.Length == 0) return Text;

               var row = new TextBlock { Foreground = PopupForeground };
               row.Inlines.Add(new Run(Text));
               row.Inlines.Add(new Run("   " + hint) { Foreground = PopupHint, FontSize = 11 });
               return row;
            }
         }
         public ImageSource Image { get { return null; } }
         public double Priority { get { return 0; } }

         public void Complete(TextArea textArea, ISegment segment, EventArgs request)
         {
            textArea.Document.Replace(segment, Text);
         }
      }
   }
}
