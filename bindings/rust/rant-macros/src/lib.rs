//! The derive macros of the Rant crate. Use them through `rant::Message` and `rant::Enum`.

use proc_macro::TokenStream;
use proc_macro2::Span;
use quote::quote;
use syn::{Data, DeriveInput, Error, Expr, ExprLit, ExprUnary, Fields, Lit, LitStr, UnOp, parse_macro_input};

/// Makes a struct with named fields a Rant message type, named as the struct or by
/// `#[rant(name = "Other")]`. Each field keeps its name, and its type must be a message type.
#[proc_macro_derive(Message, attributes(rant))]
pub fn derive_message(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    message(&input).unwrap_or_else(Error::into_compile_error).into()
}

/// Makes a field-less enum with an integer `#[repr]` a Rant enum: a named integer, named as
/// the enum or by `#[rant(name = "Other")]`, whose options keep the variants' names and values.
#[proc_macro_derive(Enum, attributes(rant))]
pub fn derive_enum(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    enumeration(&input).unwrap_or_else(Error::into_compile_error).into()
}

struct Attrs {
    name: Option<LitStr>,
    standard: bool,
}

fn attrs(input: &DeriveInput) -> syn::Result<Attrs> {
    let mut out = Attrs { name: None, standard: false };
    for attr in input.attrs.iter().filter(|a| a.path().is_ident("rant")) {
        attr.parse_nested_meta(|meta| {
            if meta.path.is_ident("name") {
                out.name = Some(meta.value()?.parse()?);
                Ok(())
            } else if meta.path.is_ident("standard") {
                out.standard = true;
                Ok(())
            } else {
                Err(meta.error("rant: the attribute takes name = \"TypeName\""))
            }
        })?;
    }
    Ok(out)
}

fn message(input: &DeriveInput) -> syn::Result<proc_macro2::TokenStream> {
    let ident = &input.ident;
    if !input.generics.params.is_empty() {
        return Err(Error::new_spanned(&input.generics, "rant: a message type has no generic parameters, since its wire name must be one name"));
    }
    let fields = match &input.data {
        Data::Struct(s) => match &s.fields {
            Fields::Named(f) => f.named.iter().collect::<Vec<_>>(),
            Fields::Unit => Vec::new(),
            Fields::Unnamed(_) => return Err(Error::new_spanned(ident, "rant: a message type needs named fields, the wire names them")),
        },
        _ => return Err(Error::new_spanned(ident, "rant: derive Message on a struct, or Enum on an enum")),
    };
    let a = attrs(input)?;
    let name = a.name.unwrap_or_else(|| LitStr::new(&ident.to_string(), ident.span()));
    let idents: Vec<_> = fields.iter().map(|f| f.ident.clone().unwrap()).collect();
    let names: Vec<_> = idents.iter().map(|i| LitStr::new(i.to_string().trim_start_matches("r#"), i.span())).collect();
    let types: Vec<_> = fields.iter().map(|f| &f.ty).collect();
    let spell = if a.standard {
        quote! { s.standard(#name, |s| { #( s.member::<#types>(#names); )* }) }
    } else {
        quote! { s.structure(::std::any::TypeId::of::<Self>(), #name, |s| { #( s.member::<#types>(#names); )* }) }
    };
    Ok(quote! {
        impl ::rant::Message for #ident {
            fn __spell(s: &mut ::rant::__private::Spell) {
                #spell
            }
            fn __encode(&self, e: &mut ::rant::__private::Encoder<'_>) -> ::std::result::Result<(), ::std::string::String> {
                #( ::rant::Message::__encode(&self.#idents, e)?; )*
                ::std::result::Result::Ok(())
            }
            fn __decode(d: &mut ::rant::__private::Decoder<'_>) -> ::std::result::Result<Self, ::std::string::String> {
                ::std::result::Result::Ok(Self { #( #idents: <#types as ::rant::Message>::__decode(d)?, )* })
            }
        }
    })
}

fn literal(e: &Expr) -> Option<i128> {
    match e {
        Expr::Lit(ExprLit { lit: Lit::Int(i), .. }) => i.base10_parse().ok(),
        Expr::Unary(ExprUnary { op: UnOp::Neg(_), expr, .. }) => literal(expr).map(|v| -v),
        _ => None,
    }
}

fn enumeration(input: &DeriveInput) -> syn::Result<proc_macro2::TokenStream> {
    let ident = &input.ident;
    let Data::Enum(data) = &input.data else {
        return Err(Error::new_spanned(ident, "rant: derive Enum on an enum, or Message on a struct"));
    };
    let mut repr = None;
    for attr in input.attrs.iter().filter(|a| a.path().is_ident("repr")) {
        attr.parse_nested_meta(|meta| {
            repr = meta.path.get_ident().map(|i| i.to_string());
            Ok(())
        })?;
    }
    let (backing, kind) = match repr.as_deref() {
        Some(r @ ("u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64")) => (syn::Ident::new(r, Span::call_site()), syn::Ident::new(&r.to_uppercase(), Span::call_site())),
        _ => return Err(Error::new_spanned(ident, "rant: an Enum needs #[repr(u8)] or another integer repr, which is its wire type")),
    };
    let mut next: i128 = 0;
    let (mut variants, mut names, mut values) = (Vec::new(), Vec::new(), Vec::new());
    for v in &data.variants {
        if !matches!(v.fields, Fields::Unit) {
            return Err(Error::new_spanned(v, "rant: an Enum's variants carry no fields"));
        }
        if let Some((_, e)) = &v.discriminant {
            next = literal(e).ok_or_else(|| Error::new_spanned(e, "rant: an Enum discriminant must be an integer literal"))?;
        }
        let value = i64::try_from(next).map_err(|_| Error::new_spanned(v, "rant: the value does not fit an i64"))?;
        variants.push(&v.ident);
        names.push(LitStr::new(&v.ident.to_string(), v.ident.span()));
        values.push(value);
        next += 1;
    }
    let as_backing: Vec<_> = values
        .iter()
        .map(|v| {
            let abs = syn::LitInt::new(&format!("{}{backing}", v.unsigned_abs()), Span::call_site());
            if *v < 0 { quote!(-#abs) } else { quote!(#abs) }
        })
        .collect();
    let a = attrs(input)?;
    let name = a.name.unwrap_or_else(|| LitStr::new(&ident.to_string(), ident.span()));
    let spell = if a.standard {
        quote! { s.standard_enum(#name, ::rant::__private::kind::#kind) }
    } else {
        quote! { s.enumeration(::std::any::TypeId::of::<Self>(), #name, ::rant::__private::kind::#kind, &[ #( (#names, #values) ),* ]) }
    };
    Ok(quote! {
        impl ::rant::Message for #ident {
            fn __spell(s: &mut ::rant::__private::Spell) {
                #spell
            }
            fn __encode(&self, e: &mut ::rant::__private::Encoder<'_>) -> ::std::result::Result<(), ::std::string::String> {
                let v: #backing = match self { #( Self::#variants => #as_backing, )* };
                e.leaf(&v.to_le_bytes())
            }
            fn __decode(d: &mut ::rant::__private::Decoder<'_>) -> ::std::result::Result<Self, ::std::string::String> {
                let b = d.leaf(::std::mem::size_of::<#backing>())?;
                let v = #backing::from_le_bytes(b.try_into().map_err(|_| "a short enum")?);
                match v {
                    #( #as_backing => ::std::result::Result::Ok(Self::#variants), )*
                    other => ::std::result::Result::Err(::std::format!("{} is no option of {}", other, ::std::stringify!(#ident))),
                }
            }
        }
    })
}
